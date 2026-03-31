#include <vector>

#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/scheduler.h"
#include "dram_controller/rowpolicy.h"

namespace Ramulator {

// OpenRowPolicy: 아무것도 안 함 → row를 그냥 열어둠
class OpenRowPolicy : public IRowPolicy, public Implementation {
  // Ramulator에 "OpenRowPolicy"라는 이름으로 등록
  RAMULATOR_REGISTER_IMPLEMENTATION(IRowPolicy, OpenRowPolicy, "OpenRowPolicy", "Open Row Policy.")
  private:
    
  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override { };

    void update(bool request_found, ReqBuffer::iterator& req_it) override { 
      // 어떤 rquest가 발행된 뒤 row policy가 update될 때 호출됨
      // 그런데 OpenRowPolicy는 아무 action도 하지 않음. 
      // PRE 강제로 넣지 x. access 끝나도 row 그냥 유지 
      // OpenRowPolicy does not need to take any actions
    };


};

// ClosedRowPolicy: 어떤 bank에서 column access가 일정 횟수(cap) 이상 쌓이면 PRE 요청을 넣어서 row를 닫음
class ClosedRowPolicy : public IRowPolicy, public Implementation {
  // "ClosedRowPolicy"라는 이름으로 등록
  RAMULATOR_REGISTER_IMPLEMENTATION(IRowPolicy, ClosedRowPolicy, "ClosedRowPolicy", "Close Row Policy.")
  private:
    IDRAM* m_dram; // DRAM 모델 포인터
    
    int m_PRE_req_id = -1; // “close-row” request의 request ID. 나중에 Request req(..., m_PRE_req_id) 만들 때 사용
    
    int m_cap = -1; // 한 bank에서 몇 번 column access가 일어나면 row를 닫을지 정하는 threshold
    // address vector에서 각 level이 몇 번째 index인지 저장
    // addr_vect[level]로 rank/bankgroup/bank/row 꺼내기 위해 필요 
    int m_rank_level = -1;
    int m_bankgroup_level = -1;
    int m_bank_level = -1;
    int m_row_level = -1;
    // 전체 구조 크기. m_col_accesses 벡터 크기 계산에 사용
    int m_num_ranks = -1;
    int m_num_bankgroups = -1;
    int m_num_banks = -1;
    // 통계용 카운터 
    int s_num_close_reqs = 0;
    // 핵심 자료 구조. 
    // 각 bank마다 지금까지 몇 번 column accesse(RD/WR)가 있었는지 카운트
    // row가 닫히면 해당 bank 카운터를 0으로 리셋
    std::vector<uint64_t> m_col_accesses;

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_ctrl = cast_parent<IDRAMController>(); // 부모 controller를 가져와서 m_ctrl에 저장
      m_dram = m_ctrl->m_dram;

      // 사실상 OpenRowpolicy와 같음. 
      m_cap = param<int>("cap").default_val(10000000); // TODO

      // address vector에서 각 계층 index를 알아냄 
      m_rank_level = m_dram->m_levels("rank");
      m_bankgroup_level = m_dram->m_levels("bankgroup");
      m_bank_level = m_dram->m_levels("bank");
      m_row_level = m_dram->m_levels("row");

      // close-row
      m_PRE_req_id = m_dram->m_requests("close-row");

      // 구조 크기 읽기. rank, bankgroup, bank 수 읽기
      m_num_ranks = m_dram->get_level_size("rank");
      m_num_bankgroups = m_dram->get_level_size("bankgroup");
      m_num_banks = m_dram->get_level_size("bank");
      
      // access counter 벡터 크기 설정. rank, bankgroup, bank 조합마다 카운터 하나씩 둠. 초기값은 전부 0 (3차원 정보 1차원으로 flatten)
      m_col_accesses.resize(m_num_banks * m_num_bankgroups * m_num_ranks, 0);
      // simulator stats에 num_close_reqs 등록
      // 시뮬 끝나면 몇 번 close-row request가 삽입됐는지 볼 수 있음 
      register_stat(s_num_close_reqs).name("num_close_reqs");
    };

    // 스케줄러가 어떤 request를 선택해서 실제 issue했을 때 호출됨
    // 방금 발행된 command/request를 보고 row policy 상태를 갱신. 필요하면 PRE 요청 추가 
    void update(bool request_found, ReqBuffer::iterator& req_it) override {

      if (!request_found) // request 없으면 할 일 없음 
        return;

      if (m_dram->m_command_meta(req_it->command).is_closing ||
          m_dram->m_command_meta(req_it->command).is_refreshing)  // PRE or REF 
      {  // 방금 발행된 command가 row 닫는 command이거나 refresh이면 
        // 해당 bank의 row가 닫혔다고 보고 access count를 0으로 리셋

        if (req_it->addr_vec[m_bankgroup_level] == -1 && req_it->addr_vec[m_bank_level] == -1) {  // all bank closes
          // rank 전체에 대한 precharge all or rank 전체 refresh -> all bank closes
          for (int b = 0; b < m_num_banks; b++) {
            for (int bg = 0; bg < m_num_bankgroups; bg++) {
              int rank_id = req_it->addr_vec[m_rank_level];
              int flat_bank_id = b + bg * m_num_banks + rank_id * m_num_banks * m_num_bankgroups; // (rank, bankgroup, bank)를 1차원 인덱스로 바꿈 
              m_col_accesses[flat_bank_id] = 0; // 해당 rank안에 모든 bankgroup, bank에 대해 access counter를 0으로 reset
            }
          }
        } else if (req_it->addr_vec[m_bankgroup_level] == -1) {  // same bank closes
          // bankgroup은 지정 안됐고, bank는 지정됨. 
          // 같은 bank_id를 가지는 여러 bankgroup에 대해 닫는 형태 
          for (int bg = 0; bg < m_num_bankgroups; bg++) {
            int bank_id = req_it->addr_vec[m_bank_level];
            int rank_id = req_it->addr_vec[m_rank_level];
            int flat_bank_id = bank_id + bg * m_num_banks + rank_id * m_num_banks * m_num_bankgroups; // (rank, bankgroup, bank)를 1차원 인덱스로 바꿈 
            m_col_accesses[flat_bank_id] = 0; // 같은 bank id를 모든 bankgroup에 대해 순회하면서 reset
          }
        } else {  // single bank closes  (PRE, VRR, RDA, WRA) - row가 닫히는 효과가 있는 command
          // 앞의 두 경우가 아니면 단일 bank close
          int flat_bank_id = req_it->addr_vec[m_bank_level] + 
                             req_it->addr_vec[m_bankgroup_level] * m_num_banks + 
                             req_it->addr_vec[m_rank_level] * m_num_banks * m_num_bankgroups;

          m_col_accesses[flat_bank_id] = 0; // 해당 rank/bankgroup/bank의 카운터만 0으로 reset
        }
      } else if (m_dram->m_command_meta(req_it->command).is_accessing)  // RD or WR
      { // bank의 access 횟수를 증가시킴 
        int flat_bank_id = req_it->addr_vec[m_bank_level] + 
                           req_it->addr_vec[m_bankgroup_level] * m_num_banks + 
                           req_it->addr_vec[m_rank_level] * m_num_banks * m_num_bankgroups;
        
        m_col_accesses[flat_bank_id]++; // 해당 bank에서 column access 1회 추가 

        if (m_col_accesses[flat_bank_id] >= m_cap) { // threshold 도달 시 row 닫음 
          Request req(req_it->addr_vec, m_PRE_req_id); // 현재 요청의 같은 주소 벡터 바탕으로 close-row 타입 request 생성
                                                      // bank에 대해 pre 요청 하나 만듦 
          m_ctrl->priority_send(req); // 이 close-row request를 controller의 priority에 보냄 (threshold 넘었으니 빨리 row 닫게 유도)
          m_col_accesses[flat_bank_id] = 0; // 요청 보낸 뒤 카운터 초기화 
          s_num_close_reqs++; // 통계 증가 
        }
      }
    };
};

}       // namespace Ramulator
