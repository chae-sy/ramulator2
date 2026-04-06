#include <iostream>
#include <unordered_set>
#include <vector>
#include <random>

#include "base/base.h"
#include "translation/translation.h"
#include "frontend/frontend.h"

namespace Ramulator {

// 가상 페이지(VPN)를 물리 페이지(PPN)에 랜덤하게 할당하는 주소 변환기
//
// 핵심 아이디어:
// - Request의 가상 주소(req.addr)가 들어오면
// - page offset을 제외한 상위 비트(VPN)를 추출하고
// - 해당 VPN이 아직 매핑되지 않았다면 랜덤한 PPN을 하나 골라 매핑함
// - 이미 매핑된 적이 있으면 기존 PPN을 그대로 사용함
//
// 즉, 일종의 매우 단순한 page table 역할을 하는 구현체라고 볼 수 있음
class RandomTranslation : public ITranslation, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(
    ITranslation,
    RandomTranslation,
    "RandomTranslation",
    "Randomly allocate physical pages to virtual pages."
  );

  // 이 translation이 속한 상위 frontend 포인터
  // → 코어 수(get_num_cores) 등을 알기 위해 사용
  IFrontEnd* m_frontend;

  protected:
    // 랜덤한 물리 페이지를 할당할 때 사용하는 난수 생성기
    std::mt19937_64 m_allocator_rng;

    // 물리 주소 공간 관련 정보
    Addr_t m_max_paddr;         // 물리 메모리의 최대 주소
    Addr_t m_pagesize;          // 페이지 크기(byte 단위)
    int    m_offsetbits;        // page offset에 해당하는 비트 수
    size_t m_num_pages;         // 전체 물리 페이지 수

    // 각 물리 페이지가 비어 있는지 여부를 저장
    // true  = 아직 할당되지 않은 free page
    // false = 이미 사용 중인 page
    std::vector<bool> m_free_physical_pages;

    // 현재 남아 있는 free physical page 개수
    size_t m_num_free_physical_pages;

    // 코어별 주소 변환 테이블
    //
    // 구조:
    //   vector< unordered_map<VPN, PPN> >
    //
    // 즉:
    // - 바깥 vector 인덱스 = source_id(코어 번호)
    // - 안쪽 unordered_map = 해당 코어의 VPN → PPN 매핑 테이블
    //
    // 왜 코어별로 따로 두냐?
    // → 서로 다른 코어가 같은 VPN 값을 써도 다른 PPN에 매핑될 수 있게 하려는 것
    using Translation_t = std::vector<std::unordered_map<Addr_t, Addr_t>>;
    Translation_t m_translation;

    // 예약된 물리 페이지 집합
    // → reserve()로 미리 점유해 둔 페이지는 랜덤 할당 대상에서 제외됨
    std::unordered_set<Addr_t> m_reserved_pages;

  public:
    // 초기화 함수
    void init() override {
      // 랜덤 할당에 사용할 시드값 읽기
      int seed = param<int>("seed").desc("The seed for the random number generator used to allocate pages.").default_val(123);
      m_allocator_rng.seed(seed);

      // 물리 주소 공간 최대값 읽기
      m_max_paddr = param<Addr_t>("max_addr").desc("Max physical address of the memory system.").required();

      // 페이지 크기(KB 단위 입력)를 byte로 변환
      // 예: 4KB 페이지라면 4 << 10 = 4096 byte
      m_pagesize = param<Addr_t>("pagesize_KB").desc("Pagesize in KB.").default_val(4) << 10;

      // page offset 비트 수 계산
      // 예: pagesize=4096이면 offsetbits=12
      m_offsetbits = calc_log2(m_pagesize);

      // 전체 물리 페이지 수 계산
      // 예: max_addr / pagesize
      m_num_pages = m_max_paddr / m_pagesize;

      // 처음에는 모든 물리 페이지가 free 상태
      m_free_physical_pages.resize(m_num_pages, true);
      m_num_free_physical_pages = m_num_pages;

      // 상위 frontend를 가져옴
      m_frontend = cast_parent<IFrontEnd>();

      // 코어 개수만큼 translation table 생성
      m_translation.resize(m_frontend->get_num_cores());

      // logger 생성
      m_logger = Logging::create_logger("RandomTranslation");
    };

    // Request의 주소를 가상 주소 → 물리 주소로 변환
    bool translate(Request& req) override {
      // 주소에서 page offset을 제외한 상위 비트 = VPN
      // 예: addr / pagesize 와 같은 의미
      Addr_t vpn = req.addr >> m_offsetbits;

      // 이 request를 보낸 source_id(코어)에 해당하는 translation table 참조
      auto& core_translation = m_translation[req.source_id];

      // 이 코어에서 현재 VPN이 이미 매핑된 적 있는지 확인
      auto target = core_translation.find(vpn);

      if (target == core_translation.end()) {
        // 아직 이 VPN에 대한 매핑이 없음
        // → 새 물리 페이지(PPN)를 하나 배정해야 함

        if (m_num_free_physical_pages == 0) {
          // 더 이상 free physical page가 없음
          // → 기존에 쓰던 물리 페이지 중 하나를 랜덤하게 재사용
          //   (실제 swap latency 같은 건 모델링하지 않음)

          Addr_t ppn_to_replace = m_allocator_rng() % m_num_pages;

          // reserved page는 재할당 대상으로 쓰지 않음
          while (m_reserved_pages.find(ppn_to_replace) != m_reserved_pages.end()) {
            ppn_to_replace = m_allocator_rng() % m_num_pages;
          }

          // 현재 VPN을 이 PPN에 매핑
          core_translation[vpn] = ppn_to_replace;

          // 경고 로그 출력
          // "물리 페이지가 부족해서 기존 페이지를 덮어쓴다"는 의미
          m_logger->warn("Swapping out PPN {} for Addr {}, VPN {}.", ppn_to_replace, req.addr, vpn);

        } else {
          // 아직 사용 가능한 free physical page가 있음
          // → 랜덤하게 하나 골라서 배정

          Addr_t ppn_to_assign = m_allocator_rng() % m_num_pages;

          // reserved page이거나, 이미 사용 중인 page면 다시 랜덤 선택
          while (
            (m_reserved_pages.find(ppn_to_assign) != m_reserved_pages.end())
            || (!m_free_physical_pages[ppn_to_assign])
          ) {
            ppn_to_assign = m_allocator_rng() % m_num_pages;
          }

          // 현재 VPN을 이 PPN에 매핑
          core_translation[vpn] = ppn_to_assign;

          // free page 개수 감소
          m_num_free_physical_pages--;
        }
      }

      // 여기까지 오면:
      // - 기존 매핑을 찾았거나
      // - 새 매핑을 하나 만든 상태임
      //
      // 이제 최종 물리 주소를 계산:
      //
      //   물리 주소 = (PPN << offsetbits) | 기존 offset
      //
      // 즉,
      // - 상위 비트는 새 물리 페이지 번호(PPN)
      // - 하위 비트(page offset)는 원래 가상 주소의 offset을 그대로 유지
      Addr_t p_addr =
        (core_translation[vpn] << m_offsetbits)
        | (req.addr & ((1 << m_offsetbits) - 1));

      // 디버그 로그 출력
      DEBUG_LOG(
        DTRANSLATE,
        m_logger,
        "Translated Addr {}, VPN {} to Addr {}, PPN {}.",
        req.addr,
        vpn,
        p_addr,
        core_translation[vpn]
      );

      // Request 안의 주소를 물리 주소로 덮어씀
      req.addr = p_addr;

      return true;
    };

    // 특정 주소가 속한 물리 페이지를 reserved page로 등록
    bool reserve(const std::string& type, Addr_t addr) override {
      // addr가 속한 페이지 번호(PPN) 계산
      Addr_t ppn = addr >> m_offsetbits;

      // reserved 집합에 추가
      // 이미 있으면 unordered_set 특성상 중복 없이 유지됨
      m_reserved_pages.insert(ppn);

      // 디버그용 출력 흔적
      // std::cout << "Reserved PPN " << ppn << "." << std::endl;

      return true;
    };

    // 최대 물리 주소 반환
    Addr_t get_max_addr() override {
      return m_max_paddr;
    };
};

}   // namespace Ramulator