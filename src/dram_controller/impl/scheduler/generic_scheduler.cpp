#include <vector>

#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/scheduler.h"

namespace Ramulator {
// Ramulator의 FR-FCFS (First-Ready First-Come-First-Serve) 스케줄러
// 👉 요청들(buffer에 있는 request들) 중에서
// 👉 "지금 당장 실행 가능한 것(ready)"을 우선 선택
// 👉 그 다음에는 "먼저 온 것(FCFS)" 선택
// 1. 각 request의 command 계산
// 2. 모든 request를 비교하면서
// 3. 가장 좋은 하나 선택
// 우선순위:
//    Ready > Arrival time
class FRFCFS : public IScheduler, public Implementation {
  // Ramulator에 이 스케줄러 등록
  RAMULATOR_REGISTER_IMPLEMENTATION(IScheduler, FRFCFS, "FRFCFS", "FRFCFS DRAM Scheduler.")
  private:
    IDRAM* m_dram; // DRAM 상태를 확인하기 위한 포인터. timing constraint 확인. ready 여부 판단

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      // 현재 컨트롤러에서 DRAM 객체 가져옴
      // Controller → DRAM 접근 가능하게 연결
      m_dram = cast_parent<IDRAMController>()->m_dram;
    };

    // 핵심 로직 👉 두 request 중 더 좋은 걸 고르는 함수
    ReqBuffer::iterator compare(ReqBuffer::iterator req1, ReqBuffer::iterator req2) override {
      // DRAM timing constraint 만족하는지 확인
      // ready = 바로 issue 가능
      // not ready = 아직 timing 때문에 못 보냄 
      bool ready1 = m_dram->check_ready(req1->command, req1->addr_vec);
      bool ready2 = m_dram->check_ready(req2->command, req2->addr_vec);

      if (ready1 ^ ready2) { // 둘 중 하나만 ready일 때
        if (ready1) { // ready 인 것 먼저
          return req1;
        } else {
          return req2;
        }
      }

      // Fallback to FCFS
      if (req1->arrive <= req2->arrive) { // 둘 다 ready / 둘 다 not ready
        return req1; // 먼저 온 요청 선택
      } else {
        return req2;
      } 
    }

    // buffer 전체에서 최고의 request 하나 선택
    ReqBuffer::iterator get_best_request(ReqBuffer& buffer) override {
      if (buffer.size() == 0) { // 요청 없으면 끝
        return buffer.end();
      }

      // 🔥 매우 중요
      for (auto& req : buffer) { // 각 request에 대해 final_command → 실제 next command로 변환
        req.command = m_dram->get_preq_command(req.final_command, req.addr_vec);
      } // row 닫힘, final_command = READ -> 실제 command ACT
        // row 열림, fianl_command = READ -> 실제 command READ

      auto candidate = buffer.begin(); // 첫 request 기준 시작 
      for (auto next = std::next(buffer.begin(), 1); next != buffer.end(); next++) {
        candidate = compare(candidate, next); // 모든 request를 pairwise 비교. 가장 좋은 request 하나 남김 
      }
      return candidate;
    }
};

}       // namespace Ramulator
