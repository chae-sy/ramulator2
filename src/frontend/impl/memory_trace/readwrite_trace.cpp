#include <filesystem>
#include <iostream>
#include <fstream>

#include "frontend/frontend.h"
#include "base/exception.h"

namespace Ramulator {

namespace fs = std::filesystem;

// Read/Write 요청을 "주소 벡터 형태"로 저장한 trace 파일을 읽어와서
// 메모리 시스템에 순차적으로 전달하는 Frontend 구현
//
// LoadStoreTrace와 차이점:
// - LoadStoreTrace는 단일 주소(Addr_t)를 읽음
// - ReadWriteTrace는 이미 계층별로 분해된 주소 벡터(AddrVec_t)를 읽음
//
// 예를 들어 DRAM 주소가
// [channel, rank, bankgroup, bank, row, col] 같은 식으로
// 미리 쪼개져 있다면, 그 값을 그대로 받아서 사용함
class ReadWriteTrace : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, ReadWriteTrace, "ReadWriteTrace", "Read/Write DRAM address vector trace.")

  private:
    // trace 한 줄에서 실제로 사용할 정보를 저장하는 구조체
    struct Trace {
      bool is_write;     // true면 write, false면 read
      AddrVec_t addr_vec; // DRAM 계층별 주소 벡터
    };

    // trace 전체를 저장하는 벡터
    std::vector<Trace> m_trace;

    // 전체 trace 길이
    size_t m_trace_length = 0;

    // 현재 몇 번째 trace 항목을 보낼지 가리키는 인덱스
    size_t m_curr_trace_idx = 0;

    // 로그 출력용 logger
    Logger_t m_logger;

  public:
    // 초기화 함수
    // → 설정 파일에서 trace 경로와 clock_ratio를 읽고,
    //   trace 파일을 메모리에 로드함
    void init() override {
      // trace 파일 경로 읽기
      std::string trace_path_str = param<std::string>("path").desc("Path to the load store trace file.").required();

      // Frontend와 MemorySystem 사이의 클럭 비율 설정
      m_clock_ratio = param<uint>("clock_ratio").required();

      // logger 생성
      m_logger = Logging::create_logger("ReadWriteTrace");

      // trace 로딩 시작 로그
      m_logger->info("Loading trace file {} ...", trace_path_str);

      // 실제 파일 파싱
      init_trace(trace_path_str);

      // 몇 줄을 로드했는지 로그 출력
      m_logger->info("Loaded {} lines.", m_trace.size());      
    };

    // 매 tick마다 호출되는 함수
    void tick() override {
      // 현재 trace 항목 가져오기
      const Trace& t = m_trace[m_curr_trace_idx];

      // 현재 요청을 메모리 시스템으로 전달
      // 이때 주소는 단일 정수 주소가 아니라 이미 분해된 addr_vec 형태로 들어감
      m_memory_system->send({
        t.addr_vec,
        t.is_write ? Request::Type::Write : Request::Type::Read
      });

      // 다음 trace 항목으로 이동
      //
      // % m_trace_length 를 쓰므로,
      // 마지막까지 가면 다시 0으로 돌아가서 반복 재생됨
      // 즉 이 frontend는 trace를 한 번만 실행하고 끝나는 구조가 아니라
      // 계속 순환하면서 반복해서 요청을 넣는 구조임
      m_curr_trace_idx = (m_curr_trace_idx + 1) % m_trace_length;
    };
    // 👉 trace를 한 번만 실행하는 게 아니라 계속 반복한다
    // trace: A B C D E
    // 실제 실행 : A B C D E A B C D E A B C ...
    //1) steady-state 분석
    //일정한 workload를 계속 넣고
    //DRAM 상태가 안정된 이후 성능 측정
    //2) bandwidth / latency saturation 측정
    //계속 요청을 넣어서 memory를 "꽉 채움"

  private:
    // trace 파일을 읽어 m_trace 벡터를 채우는 함수
    void init_trace(const std::string& file_path_str) {
      fs::path trace_path(file_path_str);

      // 파일이 존재하지 않으면 예외 발생
      if (!fs::exists(trace_path)) {
        throw ConfigurationError("Trace {} does not exist!", file_path_str);
      }

      // 파일 열기
      std::ifstream trace_file(trace_path);

      // 파일을 열 수 없으면 예외 발생
      if (!trace_file.is_open()) {
        throw ConfigurationError("Trace {} cannot be opened!", file_path_str);
      }

      std::string line;

      // 파일을 한 줄씩 읽음
      while (std::getline(trace_file, line)) {
        // 공백 기준으로 분리
        std::vector<std::string> tokens;
        tokenize(tokens, line, " ");

        // 현재 기대하는 형식:
        // R 1,2,3,4,5,6
        // 또는
        // W 0,0,7,12,1024,8
        //
        // 즉:
        // tokens[0] = R 또는 W
        // tokens[1] = 쉼표로 구분된 주소 벡터
        //
        // TODO 주석에도 적혀 있듯이,
        // 나중에는 line 번호까지 포함해서 에러 메시지를 더 자세히 만들 수 있음
        if (tokens.size() != 2) {
          throw ConfigurationError("Trace {} format invalid!", file_path_str);
        }

        // 첫 번째 토큰으로 read/write 판별
        bool is_write = false; 
        if (tokens[0] == "R") {
          is_write = false;
        } else if (tokens[0] == "W") {
          is_write = true;
        } else {
          throw ConfigurationError("Trace {} format invalid!", file_path_str);
        }

        // 두 번째 토큰은 "1,2,3,4,5" 같은 식의 주소 벡터 문자열
        // 이를 쉼표 기준으로 다시 분리
        std::vector<std::string> addr_vec_tokens;
        tokenize(addr_vec_tokens, tokens[1], ",");

        // 문자열로 된 각 주소 요소를 정수로 변환해서 AddrVec_t에 저장
        AddrVec_t addr_vec;
        for (const auto& token : addr_vec_tokens) {
          addr_vec.push_back(std::stoll(token));
        }

        // 파싱한 요청을 trace 벡터에 저장
        m_trace.push_back({is_write, addr_vec});
      }

      trace_file.close();

      // 전체 trace 길이 저장
      m_trace_length = m_trace.size();
    };

    // 종료 조건
    //
    // 현재는 무조건 true를 반환하게 되어 있음.
    // 주석의 TODO/FIXME가 말하듯이,
    // 이 구현은 아직 제대로 된 종료 조건이 없는 상태임.
    //
    // 그런데 tick()에서는 trace를 원형으로 계속 반복하므로,
    // 논리적으로도 "끝났다"는 개념이 애매함.
    // 따라서 실제 사용 시에는 이 함수가 수정되어야 할 가능성이 큼.
    bool is_finished() override {
      return true; 
    };    
};

}        // namespace Ramulator