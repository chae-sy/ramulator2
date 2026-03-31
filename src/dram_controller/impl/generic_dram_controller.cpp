#include "dram_controller/controller.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

// 요청 받기 → 큐에 넣기 → 매 cycle마다 스케줄링 → command issue → read 완료 callback 처리
// IDRAMController를 상속. DRAM controller 인터페이스 구현하는 클래스
// Implementation 상속. Ramulator 내부의 플러그인/구현체 등록 시스템과 연결 
class GenericDRAMController final : public IDRAMController, public Implementation {
  // 이 구현체를 Ramulator에 등록. 설정 파일에서 controller를 generic으로 설정하면 이 클래스가 사용됨.
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAMController, GenericDRAMController, "Generic", "A generic DRAM controller.");
  private:
    std::deque<Request> pending;          // A queue for read requests that are about to finish (callback after RL)
                                          // 이미 마지막 read command는 issue 되었지만 아직 데이터가 돌아오지 않은 요청
                                          // read latency가 지난 뒤 callback을 호출하기 전까지 여기 있음

    ReqBuffer m_active_buffer;            // Buffer for requests being served. This has the highest priority 
    ReqBuffer m_priority_buffer;          // Buffer for high-priority requests (e.g., maintenance like refresh).
    ReqBuffer m_read_buffer;              // Read request buffer
    ReqBuffer m_write_buffer;             // Write request buffer

    int m_bank_addr_idx = -1; // 주소 벡터에서 bank level이 몇 번째 index인지 저장. 나중에 같은 bank인지 비교 시 사용 

    float m_wr_low_watermark; // write draining 정책용. write queue가 너무 차면 write mode로 바꾸고 충분히 줄면 read mode로 돌아감. 
    float m_wr_high_watermark;
    bool  m_is_write_mode = false;

    // 통계용 변수
    size_t s_row_hits = 0; // 원하는 row가 이미 열려있음
    size_t s_row_misses = 0; // row는 열려 있는데 원하는 row가 아님
    size_t s_row_conflicts = 0; // row가 안 열려 있음 
    size_t s_read_row_hits = 0;
    size_t s_read_row_misses = 0;
    size_t s_read_row_conflicts = 0;
    size_t s_write_row_hits = 0;
    size_t s_write_row_misses = 0;
    size_t s_write_row_conflicts = 0;

    // core 별 read hit/miss/conflict 통계
    size_t m_num_cores = 0;
    std::vector<size_t> s_read_row_hits_per_core;
    std::vector<size_t> s_read_row_misses_per_core;
    std::vector<size_t> s_read_row_conflicts_per_core;

    // 요청 개수, queue length, 평균 read latency 같은 통계용 변수들 
    size_t s_num_read_reqs = 0;
    size_t s_num_write_reqs = 0;
    size_t s_num_other_reqs = 0;
    size_t s_queue_len = 0;
    size_t s_read_queue_len = 0;
    size_t s_write_queue_len = 0;
    size_t s_priority_queue_len = 0;
    float s_queue_len_avg = 0;
    float s_read_queue_len_avg = 0;
    float s_write_queue_len_avg = 0;
    float s_priority_queue_len_avg = 0;

    size_t s_read_latency = 0;
    float s_avg_read_latency = 0;


  public:
    void init() override { // controller 객체 생성 직후 초기화 시 호출
      m_wr_low_watermark =  param<float>("wr_low_watermark").desc("Threshold for switching back to read mode.").default_val(0.2f); // write queue가 80% 이상 차면 write mode
      m_wr_high_watermark = param<float>("wr_high_watermark").desc("Threshold for switching to write mode.").default_val(0.8f); // write queue가 20% 아래로 내려가면 다시 read mode

      // scheduler, refresh manager, row_policy 생성 (controller 혼자 다 하는게 아니라 하위 정책 모듈 붙여서)
      m_scheduler = create_child_ifce<IScheduler>();
      m_refresh = create_child_ifce<IRefreshManager>();    
      m_rowpolicy = create_child_ifce<IRowPolicy>();    

      if (m_config["plugins"]) { // 추가 controller plugin이 설정되어 있으면 생성해서 등록
        YAML::Node plugin_configs = m_config["plugins"];
        for (YAML::iterator it = plugin_configs.begin(); it != plugin_configs.end(); ++it) {
          m_plugins.push_back(create_child_ifce<IControllerPlugin>(*it));
        }
      }
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override { // frontend와 memory system이 준비된 후 연결할 때 호출
      m_dram = memory_system->get_ifce<IDRAM>(); // memory system에서 실제 DRAM model interface를 가져옴
                                                  // 이후 check_ready, issue_command, check_rowbuffer_hit 같은 DRAM 동작을 여기에 물어봄 
      m_bank_addr_idx = m_dram->m_levels("bank"); // 주소 벡터에서 bank level의 위치 저장
      m_priority_buffer.max_size = 512*3 + 32; // priority 버퍼 최대 크기 설정

      m_num_cores = frontend->get_num_cores(); // frontend의 core 개수 읽음 

      s_read_row_hits_per_core.resize(m_num_cores, 0); // core별 통계 벡터 크기 초기화
      s_read_row_misses_per_core.resize(m_num_cores, 0);
      s_read_row_conflicts_per_core.resize(m_num_cores, 0);

      // 통계 등록
      register_stat(s_row_hits).name("row_hits_{}", m_channel_id);
      register_stat(s_row_misses).name("row_misses_{}", m_channel_id);
      register_stat(s_row_conflicts).name("row_conflicts_{}", m_channel_id);
      register_stat(s_read_row_hits).name("read_row_hits_{}", m_channel_id);
      register_stat(s_read_row_misses).name("read_row_misses_{}", m_channel_id);
      register_stat(s_read_row_conflicts).name("read_row_conflicts_{}", m_channel_id);
      register_stat(s_write_row_hits).name("write_row_hits_{}", m_channel_id);
      register_stat(s_write_row_misses).name("write_row_misses_{}", m_channel_id);
      register_stat(s_write_row_conflicts).name("write_row_conflicts_{}", m_channel_id);

      for (size_t core_id = 0; core_id < m_num_cores; core_id++) {
        register_stat(s_read_row_hits_per_core[core_id]).name("read_row_hits_core_{}", core_id);
        register_stat(s_read_row_misses_per_core[core_id]).name("read_row_misses_core_{}", core_id);
        register_stat(s_read_row_conflicts_per_core[core_id]).name("read_row_conflicts_core_{}", core_id);
      }

      register_stat(s_num_read_reqs).name("num_read_reqs_{}", m_channel_id);
      register_stat(s_num_write_reqs).name("num_write_reqs_{}", m_channel_id);
      register_stat(s_num_other_reqs).name("num_other_reqs_{}", m_channel_id);
      register_stat(s_queue_len).name("queue_len_{}", m_channel_id);
      register_stat(s_read_queue_len).name("read_queue_len_{}", m_channel_id);
      register_stat(s_write_queue_len).name("write_queue_len_{}", m_channel_id);
      register_stat(s_priority_queue_len).name("priority_queue_len_{}", m_channel_id);
      register_stat(s_queue_len_avg).name("queue_len_avg_{}", m_channel_id);
      register_stat(s_read_queue_len_avg).name("read_queue_len_avg_{}", m_channel_id);
      register_stat(s_write_queue_len_avg).name("write_queue_len_avg_{}", m_channel_id);
      register_stat(s_priority_queue_len_avg).name("priority_queue_len_avg_{}", m_channel_id);

      register_stat(s_read_latency).name("read_latency_{}", m_channel_id);
      register_stat(s_avg_read_latency).name("avg_read_latency_{}", m_channel_id);
    };

    bool send(Request& req) override { // frontend에서 일반 read/write 요청을 controller에 보낼 때 호출
      req.final_command = m_dram->m_request_translations(req.type_id); // request type을 DRAM의 최종 command로 번역
      // request 하나가 내부적으로 ACT → RD 같은 여러 command를 거치더라도, 이 request의 “끝 command”가 무엇인지 알아야 하기 때문이야
      
      // 요청 종류별 개수 통계 증가
      switch (req.type_id) {
        case Request::Type::Read: {
          s_num_read_reqs++;
          break;
        }
        case Request::Type::Write: {
          s_num_write_reqs++;
          break;
        }
        default: {
          s_num_other_reqs++;
          break;
        }
      }

      // Forward existing write requests to incoming read requests
      if (req.type_id == Request::Type::Read) { // 새로 들어온 요청이 read일 때
        auto compare_addr = [req](const Request& wreq) { 
          return wreq.addr == req.addr; // write buffer 안에 같은 주소로 가는 write가 이미 있으면 굳이 DRAM까지 read를 보내지 않음 
                                        // 이 write가 곧 최신값을 가지고 있으니 read는 forwarding으로 해결 가능하다고 보는 것
        };
        if (std::find_if(m_write_buffer.begin(), m_write_buffer.end(), compare_addr) != m_write_buffer.end()) {
          // The request will depart at the next cycle
          req.depart = m_clk + 1; // 다음 cycle에 끝난 것처럼 처리
          pending.push_back(req); // pending에 넣어뒀다가 serve_completed_reads()에서 callback 호출
          return true;
        }
      }

      // Else, enqueue them to corresponding buffer based on request type id
      bool is_success = false;
      req.arrive = m_clk; // enqueue 시도 전에 도착 cycle 기록
      if        (req.type_id == Request::Type::Read) {
        is_success = m_read_buffer.enqueue(req); // read면 read buffer
      } else if (req.type_id == Request::Type::Write) {
        is_success = m_write_buffer.enqueue(req); // write면 write buffer
      } else {
        throw std::runtime_error("Invalid request type!");
      }
      if (!is_success) { // 큐가 가득 차서 enqueue 실패하면 false 반환
        // We could not enqueue the request
        req.arrive = -1;
        return false;
      }

      return true; // enqueue 성공 시 
    };

    bool priority_send(Request& req) override {
      // refresh 등 고우선 요청을 priority buffer에 넣음.
      // 일반 send와 비슷하지만 read/write buffer가 아니라 priority buffer로 감 
      req.final_command = m_dram->m_request_translations(req.type_id);

      bool is_success = false;
      is_success = m_priority_buffer.enqueue(req);
      return is_success;
    }

    void tick() override {
      m_clk++; // controller clock 1 증가

      // Update statistics
      s_queue_len += m_read_buffer.size() + m_write_buffer.size() + m_priority_buffer.size() + pending.size(); // 매 cycle의 queue 길이 누적
      s_read_queue_len += m_read_buffer.size() + pending.size(); // pending 포함. 즉, 아직 callback 안 된 read도 read-side 부담으로 보겠다. 
      s_write_queue_len += m_write_buffer.size();
      s_priority_queue_len += m_priority_buffer.size();

      // 1. Serve completed reads
      serve_completed_reads(); // 이미 마지막 read command는 나갔고, latency만 기다리는 요청들 중 완료된 게 있으면 callback 호출

      m_refresh->tick(); // refresh manager도 매 cycle 업데이트

      // 2. Try to find a request to serve.
      // scheduler와 각 큐 상태를 바탕으로 이번 cycle에 실제 cocmmand를 낼 요청 하나 선택
      ReqBuffer::iterator req_it; 
      ReqBuffer* buffer = nullptr;
      bool request_found = schedule_request(req_it, buffer); // req_it: 선택된 요청, buffer: 그 요청이 들어있는 버퍼, request_found: 찾았는지 여부 

      // 2.1 Take row policy action
      m_rowpolicy->update(request_found, req_it); // open-page, close-page 같은 row policy에 따라 내부 상태 업데이트
                                                  // 선택된 요청이 있든 없든 policy는 매 cycle 상태를 볼 수 있음. 

      // 3. Update all plugins
      for (auto plugin : m_plugins) {
        plugin->update(request_found, req_it);
      }

      // 4. Finally, issue the commands to serve the request
      if (request_found) { // 이번 cycle에 실제로 낼 commmand가 있으면 들어감
        // If we find a real request to serve
        if (req_it->is_stat_updated == false) {
          update_request_stats(req_it); // 이 request에 대해 row hit/miss/conflict 통계가 아직 안찍혔으면 여기서 1회만 찍음
                                        // request가 여러 command를 거치더라도 통계는 한 번만 갱신하려는 목적
        }
        m_dram->issue_command(req_it->command, req_it->addr_vec); // ⭐진짜 DRAM command issue 지점⭐

        // If we are issuing the last command, set depart clock cycle and move the request to the pending queue
        if (req_it->command == req_it->final_command) { // 지금 issue한 command가 이 request의 마지막 command면 이 request는 DRAM 관점에서 거의 끝난 상태
          if (req_it->type_id == Request::Type::Read) { // read인 경우
            req_it->depart = m_clk + m_dram->m_read_latency; // read 데이터가 read latency 뒤에 돌아오므로 depart를 현재 cycle + read_latency로 설정
            pending.push_back(*req_it); // pending에 넣어서 나중에 완료 처리
          } else if (req_it->type_id == Request::Type::Write) {
            // TODO: Add code to update statistics
            // 보통 write는 callback 기반 응답이 read만큼 강조되지 않는 경우가 많음
          }
          buffer->remove(req_it); // 원래 있던 큐에서 이 요청 제거 
        } else {
          if (m_dram->m_command_meta(req_it->command).is_opening) { // 아직 마지막 command가 아니면 
            if (m_active_buffer.enqueue(*req_it)) { // 지금 command가 ACT처럼 row 여는 command면 이후 이어서 처리해야하므로 request를 m_active_buffer로 옮김
              buffer->remove(req_it); 
            }
          }
        }

      }

    };


  private:
    /**
     * @brief    Helper function to check if a request is hitting an open row
     * @details
     * 
     */
    bool is_row_hit(ReqBuffer::iterator& req) // 이 요청이 현재 열린 row에 바로 hit하는지 확인
    {
        return m_dram->check_rowbuffer_hit(req->final_command, req->addr_vec);
    }
    /**
     * @brief    Helper function to check if a request is opening a row
     * @details
     * 
    */
    bool is_row_open(ReqBuffer::iterator& req) // 어떤 row든 열려 있는지 확인. hit은 아니지만 row 열려있으면 conflict 가능성
    {
        return m_dram->check_node_open(req->final_command, req->addr_vec);
    }

    /**
     * @brief    
     * @details
     * 
     */
    void update_request_stats(ReqBuffer::iterator& req) // request를 처음 서비스할 때 row locality 통계 갱신
    {
      req->is_stat_updated = true; // 같은 request에 대해 중복 통계 갱신 방지

      if (req->type_id == Request::Type::Read)  // READ일때
      {
        if (is_row_hit(req)) { // row hit
          s_read_row_hits++; // read hit 증가
          s_row_hits++; // 전체 hit 증가 
          if (req->source_id != -1)
            s_read_row_hits_per_core[req->source_id]++; // core별 hit 증가 
        } else if (is_row_open(req)) { // row 열려 있지만 원하는 row가 아닌 경우 conflict
          s_read_row_conflicts++;
          s_row_conflicts++;
          if (req->source_id != -1)
            s_read_row_conflicts_per_core[req->source_id]++;
        } else { // row 자체가 열리지 않은 경우 miss
          s_read_row_misses++;
          s_row_misses++;
          if (req->source_id != -1)
            s_read_row_misses_per_core[req->source_id]++;
        } 
      } 
      else if (req->type_id == Request::Type::Write) // WRITE일 때
      {
        if (is_row_hit(req)) {
          s_write_row_hits++;
          s_row_hits++;
        } else if (is_row_open(req)) {
          s_write_row_conflicts++;
          s_row_conflicts++;
        } else {
          s_write_row_misses++;
          s_row_misses++;
        }
      }
    }

    /**
     * @brief    Helper function to serve the completed read requests
     * @details
     * This function is called at the beginning of the tick() function.
     * It checks the pending queue to see if the top request has received data from DRAM.
     * If so, it finishes this request by calling its callback and poping it from the pending queue.
     * 데이터가 돌아온 read를 외부에 완료 통보하는 함수
     */
    void serve_completed_reads() { // pending queue가 비어있지 않으면 맨 앞 요청 확인 
      if (pending.size()) {
        // Check the first pending request
        auto& req = pending[0]; 
        if (req.depart <= m_clk) { // 현재 cycle이 depart 시각 이상이면 완료된 것
          // Request received data from dram
          if (req.depart - req.arrive > 1) {
            // Check if this requests accesses the DRAM or is being forwarded.
            // TODO add the stats back
            s_read_latency += req.depart - req.arrive; // *실제 DRAM 접근이 있었던 read*만 latency 통계에 더하려는 느낌
          }

          if (req.callback) {
            // If the request comes from outside (e.g., processor), call its callback
            // 요청 보낸 쪽에 이 read 끝났다고 알려줌 
            req.callback(req);
          }
          // Finally, remove this request from the pending queue
          pending.pop_front();
        }
      };
    };


    /**
     * @brief    Checks if we need to switch to write mode
     * @details
     * high/low watermark를 따로 둬서 mode가 read↔write로 계속 흔들리는 걸 막음.
     */
    void set_write_mode() {
      if (!m_is_write_mode) { // 현재 read mode면 write mode로 바꿀지 검사
        if ((m_write_buffer.size() > m_wr_high_watermark * m_write_buffer.max_size) || m_read_buffer.size() == 0) { // write queue가 너무 많거나 read가 하나도 없을 때
          m_is_write_mode = true; // write mode 진입 
        }
      } else { // 이미 write mode라면 
        if ((m_write_buffer.size() < m_wr_low_watermark * m_write_buffer.max_size) && m_read_buffer.size() != 0) { // write queue가 충분히 줄고 read가 존재할 때
          m_is_write_mode = false; // read mode로 복귀
        }
      }
    };


    /**
     * @brief    Helper function to find a request to schedule from the buffers.
     * @details
     * ⭐ 이번 cycle에 뭘 낼지 정하는 핵심 
     */
    bool schedule_request(ReqBuffer::iterator& req_it, ReqBuffer*& req_buffer) {
      bool request_found = false; 

      // 2.1    First, check the **act buffer** to serve requests that are already activating (avoid useless ACTs)
      if (req_it= m_scheduler->get_best_request(m_active_buffer); req_it != m_active_buffer.end()) {
        if (m_dram->check_ready(req_it->command, req_it->addr_vec)) {
          request_found = true;
          req_buffer = &m_active_buffer;
        }
      }

      // 2.2    If no requests can be scheduled from the act buffer, check the rest of the buffers
      if (!request_found) {
        // 2.2.1    We first check the priority buffer to prioritize e.g., maintenance requests
        if (m_priority_buffer.size() != 0) {
          req_buffer = &m_priority_buffer; // priority buffer는 scheduler를 안 쓰고 맨 앞 요청을 그대로 봄
          req_it = m_priority_buffer.begin();
          req_it->command = m_dram->get_preq_command(req_it->final_command, req_it->addr_vec); // 이 request의 다음 필요한 command
          // 예를 들어 final command가 RD라도, 지금 row가 안 열려 있으면 실제로 먼저 필요한 건 ACT일 수 있음.
          
          request_found = m_dram->check_ready(req_it->command, req_it->addr_vec); // ready면 바로 이 priority request를 서비스
          if (!request_found & m_priority_buffer.size() != 0) { // ready가 아니면 false 반환하고 끝. 
            // priority buffer에 요청이 있는데 준비가 안되어 있으면 다른 일반 read/write으로 넘어가지 않고 그냥 그 cycle은 발행 안함 
            return false;
          }
        }

        // 2.2.1    If no request to be scheduled in the priority buffer, check the read and write buffers.
        if (!request_found) {
          // Query the write policy to decide which buffer to serve
          set_write_mode(); // 현재 mode에 따라 read buffer 또는 write buffer 선택 
          auto& buffer = m_is_write_mode ? m_write_buffer : m_read_buffer; 
          if (req_it = m_scheduler->get_best_request(buffer); req_it != buffer.end()) { // 해당 buffer에서 scheduler가 best request 선택, ready이면 서비스 대상으로 확정
            request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
            req_buffer = &buffer;
          }
        }
      }

      // 2.3 If we find a request to schedule, we need to check if it will close an opened row in the active buffer.
      if (request_found) {
        if (m_dram->m_command_meta(req_it->command).is_closing) { // 뽑은 command가 row를 닫는 command면 추가 검사 수행
          // 닫으려는 bank/rowgroup이
          // active buffer에 있는 진행 중 요청과 같은 bank 범위라면
          // 그 row를 닫으면 active request 진행을 방해할 수 있음
          // 그래서 이번 cycle엔 그 closing command를 발행하지 않음
          auto& rowgroup = req_it->addr_vec;
          for (auto _it = m_active_buffer.begin(); _it != m_active_buffer.end(); _it++) {
            auto& _it_rowgroup = _it->addr_vec;
            bool is_matching = true;
            for (int i = 0; i < m_bank_addr_idx + 1 ; i++) {
              if (_it_rowgroup[i] != rowgroup[i] && _it_rowgroup[i] != -1 && rowgroup[i] != -1) {
                is_matching = false;
                break;
              }
            }
            if (is_matching) {
              request_found = false;
              break;
            }
          }
        }
      }

      return request_found;
    }

    void finalize() override {
      // 총 read latency를 read 개수로 나눠 평균 latency 계산
      s_avg_read_latency = (float) s_read_latency / (float) s_num_read_reqs;
      // cycle 동안 누적한 queue length를 clock 수로 나눠 평균 queue length 계산 
      s_queue_len_avg = (float) s_queue_len / (float) m_clk;
      s_read_queue_len_avg = (float) s_read_queue_len / (float) m_clk;
      s_write_queue_len_avg = (float) s_write_queue_len / (float) m_clk;
      s_priority_queue_len_avg = (float) s_priority_queue_len / (float) m_clk;

      return;
    }

};
  
}   // namespace Ramulator
