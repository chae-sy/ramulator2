#include "memory_system/memory_system.h"
#include "translation/translation.h"
#include "dram_controller/controller.h"
#include "addr_mapper/addr_mapper.h"
#include "dram/dram.h"

namespace Ramulator {

// 일반적인 DRAM 기반 메모리 시스템 구현체
// → Frontend가 보낸 Request를 받아서
//   주소 매핑을 수행하고,
//   해당 channel의 controller로 전달하는 역할
class GenericDRAMSystem final : public IMemorySystem, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(
    IMemorySystem,
    GenericDRAMSystem,
    "GenericDRAM",
    "A generic DRAM-based memory system."
  );

  protected:
    // 현재 메모리 시스템 cycle 수
    Clk_t m_clk = 0;

    // DRAM 전체 장치 인터페이스
    // 보통 top-level DRAM node이며, 내부에 channel/rank/bank 등이 연결됨
    IDRAM*  m_dram;

    // 주소 매핑기
    // 평면 주소(Addr_t)를 channel/rank/bank/row/col 같은 addr_vec으로 변환하는 역할
    IAddrMapper*  m_addr_mapper;

    // channel별 DRAM controller들
    // channel 수만큼 생성됨
    std::vector<IDRAMController*> m_controllers;

  public:
    // 통계용 카운터
    // 성공적으로 받아들여진 요청 수를 타입별로 셈
    int s_num_read_requests = 0;
    int s_num_write_requests = 0;
    int s_num_other_requests = 0;

  public:
    void init() override { 
      // DRAM 장치 생성
      // (모든 channel node를 감싸는 top-level device)
      m_dram = create_child_ifce<IDRAM>();

      // 주소 매핑기 생성
      m_addr_mapper = create_child_ifce<IAddrMapper>();

      // DRAM에서 channel 개수를 읽음
      int num_channels = m_dram->get_level_size("channel");   

      // channel 개수만큼 memory controller 생성
      for (int i = 0; i < num_channels; i++) {
        IDRAMController* controller = create_child_ifce<IDRAMController>();

        // 각 controller에 이름 부여
        controller->m_impl->set_id(fmt::format("Channel {}", i));

        // 이 controller가 담당하는 channel 번호 저장
        controller->m_channel_id = i;

        // controller 목록에 추가
        m_controllers.push_back(controller);
      }

      // Frontend와 MemorySystem 사이의 클럭 비율 설정
      m_clock_ratio = param<uint>("clock_ratio").required();

      // 통계 등록
      // m_clk                   → memory_system_cycles
      // s_num_read_requests     → total_num_read_requests
      // s_num_write_requests    → total_num_write_requests
      // s_num_other_requests    → total_num_other_requests
      register_stat(m_clk).name("memory_system_cycles");
      register_stat(s_num_read_requests).name("total_num_read_requests");
      register_stat(s_num_write_requests).name("total_num_write_requests");
      register_stat(s_num_other_requests).name("total_num_other_requests");
    };

    // setup 함수
    // 현재 구현에서는 별도 작업 없음
    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override { }

    // Frontend가 보낸 Request를 MemorySystem이 받아들이는 함수
    bool send(Request req) override {
      // 먼저 주소 매핑 수행
      // 예: 평면 주소를 [channel, rank, bank, row, col] 형태로 변환
      m_addr_mapper->apply(req);

      // addr_vec의 첫 번째 원소를 channel ID로 사용
      int channel_id = req.addr_vec[0];

      // 해당 channel의 controller에 요청 전달
      bool is_success = m_controllers[channel_id]->send(req);

      // controller가 요청을 성공적으로 받아들였을 때만
      // 전체 요청 통계를 증가시킴
      if (is_success) {
        switch (req.type_id) {
          case Request::Type::Read: {
            // 여기서 total_num_read_requests가 증가함
            s_num_read_requests++;
            break;
          }
          case Request::Type::Write: {
            // 여기서 total_num_write_requests가 증가함
            s_num_write_requests++;
            break;
          }
          default: {
            // Read/Write 이외의 요청 타입
            s_num_other_requests++;
            break;
          }
        }
      }

      // 요청이 controller에 받아들여졌는지 여부 반환
      return is_success;
    };
    
    // 메모리 시스템을 한 cycle 진행
    void tick() override {
      // 전체 memory system cycle 증가
      m_clk++;

      // DRAM 내부 상태 한 cycle 진행
      m_dram->tick();

      // 모든 channel controller를 한 cycle씩 진행
      for (auto controller : m_controllers) {
        controller->tick();
      }
    };

    // DRAM의 clock period(tCK)를 ps 단위에서 ns 단위로 바꿔 반환
    float get_tCK() override {
      return m_dram->m_timing_vals("tCK_ps") / 1000.0f;
    }

    // const SpecDef& get_supported_requests() override {
    //   return m_dram->m_requests;
    // };
};
  
}   // namespace