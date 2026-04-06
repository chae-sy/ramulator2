#include <vector>

#include "base/base.h"
#include "dram/dram.h"
#include "addr_mapper/addr_mapper.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

// 선형 주소 매퍼들의 공통 기반 클래스
//
// 역할:
// - DRAM organization 정보를 읽어서
//   각 계층(channel/rank/bank/row/column)에 몇 비트가 필요한지 계산
// - 실제 매핑 클래스(ChRaBaRoCo, RoBaRaCoCh, MOP4CLXOR)가
//   이 정보를 바탕으로 req.addr를 req.addr_vec으로 분해할 수 있게 준비
class LinearMapperBase : public IAddrMapper {
  public:
    IDRAM* m_dram = nullptr;   // 연결된 DRAM 인터페이스

    int m_num_levels = -1;          // DRAM 계층 수 (예: channel, rank, bank, row, col)
    std::vector<int> m_addr_bits;   // 각 계층이 차지하는 주소 비트 수
    Addr_t m_tx_offset = -1;        // transaction 단위 offset 비트 수

    int m_col_bits_idx = -1;        // column level의 인덱스
    int m_row_bits_idx = -1;        // row level의 인덱스

  protected:
    // 공통 setup 함수
    // → DRAM 조직 정보를 보고 각 계층별 주소 비트 수를 계산함
    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) {
      // 현재 memory system에 연결된 DRAM 객체 가져오기
      m_dram = memory_system->get_ifce<IDRAM>();

      // DRAM organization(count)로부터 각 계층 크기를 읽음
      // 예: [num_channels, num_ranks, num_banks, num_rows, num_cols]
      const auto& count = m_dram->m_organization.count;

      // 전체 계층 수 저장
      m_num_levels = count.size();

      // 각 계층에 필요한 주소 비트 수 계산
      // 예: count[level] = 8이면 log2(8)=3비트 필요
      m_addr_bits.resize(m_num_levels);
      for (size_t level = 0; level < m_addr_bits.size(); level++) {
        m_addr_bits[level] = calc_log2(count[level]);
      }

      // 마지막 계층(Column)은 실제로는 prefetch granularity를 가짐
      // ⭐그래서 내부 prefetch 크기만큼 column 비트 수를 줄여야 함
      //
      // 예:
      // column 개수 자체는 많더라도,
      // 한 번의 DRAM access가 prefetch 단위로 묶여 처리되므로
      // 가장 하위 일부 column 비트는 transaction 단위에서 의미가 사라짐
      m_addr_bits[m_num_levels - 1] -= calc_log2(m_dram->m_internal_prefetch_size);

      // 한 transaction이 몇 byte인지 계산
      //“한 번의 READ/WRITE로 실제 몇 바이트가 이동하느냐”
      // tx_bytes = prefetch_size * channel_width / 8
      //
      // 예:
      // - prefetch_size = 8
      // - channel_width = 64 bit
      // → tx_bytes = 8 * 64 / 8 = 64B
      int tx_bytes =
                    m_dram->m_internal_prefetch_size   // 한 번에 몇 번(beat) 읽는지 DDR4는 보통 8n prefetch → 8번 전송
                    * m_dram->m_channel_width         // 한 beat당 몇 비트인지
                    / 8;                              // 비트 → 바이트 변환

      // transaction 내부 offset 비트 수
      // 예: tx_bytes=64B면 offset=6비트
      m_tx_offset = calc_log2(tx_bytes);

      // row level이 몇 번째 인덱스인지 찾음
      // ChRaBaRoCo / RoBaRaCoCh 매핑에서 row 위치를 알기 위해 필요
      try {
        m_row_bits_idx = m_dram->m_levels("row");
      } catch (const std::out_of_range& r) {
        throw std::runtime_error(fmt::format("Organization \"row\" not found in the spec, cannot use linear mapping!"));
      }

      // column은 항상 마지막 level이라고 가정
      m_col_bits_idx = m_num_levels - 1;
    }

};


// 가장 단순한 선형 매핑 방식
//
// 이름 뜻:
//   Ch-Ra-Ba-Ro-Co
//   channel → rank → bank → row → column 순으로 주소 비트를 배치
//
// 즉 상위 주소 비트부터 순서대로 자연스럽게 각 계층에 나눠 넣는 방식
class ChRaBaRoCo final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, ChRaBaRoCo, "ChRaBaRoCo", "Applies a trival mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      // 주소 벡터 크기 확보, 초기값은 -1
      req.addr_vec.resize(m_num_levels, -1);

      // transaction 내부 offset 비트 제거
      // 즉 cache line / burst 내부 offset은 떼고,
      // 실제 DRAM 주소 계층 분해에 필요한 상위 비트만 사용
      Addr_t addr = req.addr >> m_tx_offset;

      // 뒤에서부터(보통 column부터) 차례대로 하위 비트를 잘라서 넣음
      //
      // slice_lower_bits(addr, n)은:
      // - addr의 하위 n비트를 꺼내고
      // - addr 자체도 그만큼 오른쪽으로 줄이는 동작으로 이해하면 됨
      //
      // 결과적으로 주소 비트를
      // [channel][rank][bank][row][col] 순으로 깔끔하게 나누게 됨
      for (int i = m_addr_bits.size() - 1; i >= 0; i--) {
        req.addr_vec[i] = slice_lower_bits(addr, m_addr_bits[i]);
      }
    }
};


// RoBaRaCoCh 매핑 방식
//
// 이름 뜻:
//   Ro-Ba-Ra-Co-Ch
//   row → bank → rank → column → channel 순으로
//   "낮은 비트부터" 각 계층에 배정하는 효과를 냄
//
// 중요한 특징:
// - channel 비트를 가장 낮은 쪽에 둠
// - row 비트가 상대적으로 높은 쪽으로 밀림
//
// 이 매핑은 연속 주소가 channel/bank/rank 쪽으로 먼저 퍼지고,
// row는 더 느리게 증가하게 만드는 성향이 있음
class RoBaRaCoCh final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, RoBaRaCoCh, "RoBaRaCoCh", "Applies a RoBaRaCoCh mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      // 주소 벡터 초기화
      req.addr_vec.resize(m_num_levels, -1);

      // transaction offset 제거
      Addr_t addr = req.addr >> m_tx_offset;

      // 가장 먼저 channel 비트를 꺼냄
      // → 가장 낮은 비트를 channel에 할당
      req.addr_vec[0] = slice_lower_bits(addr, m_addr_bits[0]);

      // 가장 마지막 level(column)도 따로 꺼냄
      req.addr_vec[m_addr_bits.size() - 1] = slice_lower_bits(addr, m_addr_bits[m_addr_bits.size() - 1]);

      // 그 사이 level들(rank, bank, row 등)을 순서대로 채움
      //
      // 여기서 핵심은:
      // 낮은 비트부터 channel, column, rank/bank/... 순으로 잘라 먹기 때문에
      // ChRaBaRoCo와는 전혀 다른 주소 분포를 만들 수 있다는 점
      for (int i = 1; i <= m_row_bits_idx; i++) {
        req.addr_vec[i] = slice_lower_bits(addr, m_addr_bits[i]);
      }
    }
};


// MOP4CLXOR 매핑 방식
//
// 비교적 복잡한 XOR 기반 매핑
// → 단순 선형 분해 대신 column 일부와 row/bank 관련 비트를 XOR 섞어서
//   접근이 특정 bank/row에 몰리는 것을 줄이려는 목적
//
// 이런 방식은 bank-level parallelism을 높이거나
// row/bank hotspot을 완화하는 데 사용될 수 있음
class MOP4CLXOR final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, MOP4CLXOR, "MOP4CLXOR", "Applies a MOP4CLXOR mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      // 주소 벡터 초기화
      req.addr_vec.resize(m_num_levels, -1);

      // transaction offset 제거
      Addr_t addr = req.addr >> m_tx_offset;

      // column의 하위 2비트를 먼저 꺼냄
      req.addr_vec[m_col_bits_idx] = slice_lower_bits(addr, 2);

      // row 이전의 level들(channel/rank/bank 등)을 먼저 채움
      for (int lvl = 0 ; lvl < m_row_bits_idx ; lvl++)
          req.addr_vec[lvl] = slice_lower_bits(addr, m_addr_bits[lvl]);

      // column의 나머지 비트를 이어서 붙임
      req.addr_vec[m_col_bits_idx] += slice_lower_bits(addr, m_addr_bits[m_col_bits_idx]-2) << 2;

      // 남은 상위 비트 전체를 row로 사용
      req.addr_vec[m_row_bits_idx] = (int) addr;

      // column 비트 일부를 이용해
      // channel/rank/bank 등의 하위 주소를 XOR 섞음
      //
      // 목적:
      // - 특정 패턴의 연속 주소가 한 bank/row에 집중되는 것을 완화
      // - 더 고르게 분산되도록 함
      int row_xor_index = 0; 
      for (int lvl = 0 ; lvl < m_col_bits_idx ; lvl++){
        if (m_addr_bits[lvl] > 0){
          int mask = (req.addr_vec[m_col_bits_idx] >> row_xor_index) & ((1<<m_addr_bits[lvl])-1);

          // 해당 level 값에 XOR 적용
          req.addr_vec[lvl] = req.addr_vec[lvl] xor mask;

          row_xor_index += m_addr_bits[lvl];
        }
      }
    }
};

}   // namespace Ramulator