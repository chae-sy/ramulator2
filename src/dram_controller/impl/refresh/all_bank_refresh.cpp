#include <vector>

#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/refresh.h"

namespace Ramulator {

// Ramulator의 All-Bank Refresh manager 구현
// 일정 주기마다 각 rank에 *all-bank* refresh 요청을 넣는 역할
// nREFI 주기마다
// 각 rank에 대해 refresh request 생성
// → controller에 priority로 보냄
class AllBankRefresh : public IRefreshManager, public Implementation { 
  // IRefreshManager 인터페이스를 구현하는 refresh manager
  // Implementation을 통해 Ramulator plugin 시스템에 등록됨
  // 구현을 "AllBank"라는 이름으로 등록
  RAMULATOR_REGISTER_IMPLEMENTATION(IRefreshManager, AllBankRefresh, "AllBank", "All-Bank Refresh scheme.")
  private:
    Clk_t m_clk = 0;
    IDRAM* m_dram; // DRAM 모델 포인터 
    IDRAMController* m_ctrl; // refresh manager가 붙어있는 controller 포인터

    int m_dram_org_levels = -1; // DRAM address vector 길이 ([channel, rank, bankgroup, bank, row, col])
    int m_num_ranks = -1; // rank 개수

    int m_nrefi = -1; // refresh interval 
    int m_ref_req_id = -1; // all-bank-refresh request의 ID
    Clk_t m_next_refresh_cycle = -1; // 다음 refresh를 넣어야하는 cycle 

  public:
    void init() override { 
      m_ctrl = cast_parent<IDRAMController>(); // 이 manager의 부모 controller를 가져옴. refresh manager → controller 연결
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_dram = m_ctrl->m_dram; // controller가 가지고 있는 DRAM 객체 포인터를 가져옴

      m_dram_org_levels = m_dram->m_levels.size(); // address hierarchy의 level 수 저장 (channel, rank, bankgroup, bank, row, column 등 전체 길이)
                                                  // 나중에 std::vector<int> addr_vec(m_dram_org_levels, -1) 할 때 필요
      m_num_ranks = m_dram->get_level_size("rank"); // rank 수 저장. 왜 필요하냐면 refresh 시점마다 모든 rank에 대해 request를 넣어야 하기 때문

      m_nrefi = m_dram->m_timing_vals("nREFI"); // DRAM timing parameter 중 nREFI 값을 읽음 
      m_ref_req_id = m_dram->m_requests("all-bank-refresh"); // "all-bank-refresh" request 종류의 ID를 얻음
                                                            // 나중에 Request req(addr_vec, m_ref_req_id); 로 refresh request 객체 생성 가능

      m_next_refresh_cycle = m_nrefi; // 첫 refresh 시점을 nREFI cycle로 설정
                                      // 시뮬 시작 직후가 아니라, 처음엔 nREFI만큼 지난 뒤 refresh 시작
    };

    void tick() {
      m_clk++;

      if (m_clk == m_next_refresh_cycle) { // 지금 cycle이 refresh 시점인지 검사 
        m_next_refresh_cycle += m_nrefi; // 다음 refresh 시점을 미리 갱신
        for (int r = 0; r < m_num_ranks; r++) { // 모든 rank에 대해 refresh request 생성
          std::vector<int> addr_vec(m_dram_org_levels, -1); // address vector를 전체 길이만큼 만들고 모두 -1로 초기화
          addr_vec[0] = m_ctrl->m_channel_id; // 이 controller가 담당하는 channel ID를 넣음. 즉 이 refresh request는 현재 channel에 대한 요청
          addr_vec[1] = r; // 두 번째 level은 rank라고 보고 현재 rank r를 넣음
          // [channel = 현재 채널, rank = r, 나머지는 wildcard]
          Request req(addr_vec, m_ref_req_id); // 현재 channel의 rank r에 대해 all-bank-refresh 요청 생성

          bool is_success = m_ctrl->priority_send(req); // 이 refresh request를 controller에 *priority*로 보냄
          if (!is_success) {
            throw std::runtime_error("Failed to send refresh!");
            // refresh를 못 넣는 건 시뮬레이터 입장에선 심각한 오류
          }
        }
      }
    };

};

}       // namespace Ramulator
