#include <filesystem>
#include <iostream>
#include <fstream>

#include "frontend/frontend.h"
#include "base/exception.h"

namespace Ramulator {

namespace fs = std::filesystem;

// 파일로부터 load/store trace를 읽어와서
// 메모리 시스템에 순차적으로 요청을 넣는 Frontend 구현
class LoadStoreTrace : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, LoadStoreTrace, "LoadStoreTrace", "Load/Store memory address trace.")

  private:
    // trace 파일의 한 줄에서 실제로 사용할 최소 정보만 저장하는 구조체
    struct Trace {
      bool is_write;  // true면 write, false면 read
      Addr_t addr;    // 접근할 메모리 주소
    };

    // 전체 trace를 메모리에 저장해 두는 벡터
    std::vector<Trace> m_trace;

    // trace 전체 길이
    size_t m_trace_length = 0;

    // 현재 몇 번째 trace 요청을 보낼 차례인지 나타내는 인덱스
    size_t m_curr_trace_idx = 0;

    // 실제로 메모리 시스템에 성공적으로 전달한 요청 개수
    // 주의: tick이 호출된 횟수와는 다르고,
    // send()가 성공했을 때만 증가함
    size_t m_trace_count = 0;

    // 로그 출력용 logger
    Logger_t m_logger;

  public:
    // 초기화 함수
    // → 설정 파일에서 trace 파일 경로와 clock_ratio를 읽고,
    //   trace 파일 전체를 메모리에 로드함
    void init() override {
      // trace 파일 경로 읽기
      std::string trace_path_str = param<std::string>("path").desc("Path to the load store trace file.").required();

      // Frontend와 MemorySystem 사이의 클럭 비율 읽기
      m_clock_ratio = param<uint>("clock_ratio").required();

      // logger 생성
      m_logger = Logging::create_logger("LoadStoreTrace");

      // trace 파일 로딩 시작 로그
      m_logger->info("Loading trace file {} ...", trace_path_str);

      // 실제 trace 파일 파싱
      init_trace(trace_path_str);

      // 몇 줄을 읽었는지 로그 출력
      m_logger->info("Loaded {} lines.", m_trace.size());
    };

    // 매 cycle(혹은 frontend tick)마다 호출되는 함수
    void tick() override {
      // trace가 비어 있으면 아무것도 하지 않음
      if (m_trace_length == 0) {
        return;
      }

      // 현재 보낼 trace 항목 참조
      const Trace& t = m_trace[m_curr_trace_idx];

      // 현재 trace 항목을 메모리 시스템으로 전송 시도
      // is_write가 true면 Write request, 아니면 Read request 생성
      bool request_sent = m_memory_system->send(
        {t.addr, t.is_write ? Request::Type::Write : Request::Type::Read}
      );

      // ⭐요청 전송에 성공했을 때만 다음 trace로 넘어감
      // 즉, 메모리 시스템 queue가 꽉 차서 send()가 실패하면
      // 같은 요청을 다음 tick에 다시 시도하게 됨
      if (request_sent) {
        m_curr_trace_idx++;  // 다음 trace 항목으로 이동
        m_trace_count++;     // 성공적으로 보낸 요청 수 증가
      }
    };

  private:
    // trace 파일을 읽어서 m_trace 벡터를 채우는 함수
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
      size_t line_num = 0;

      // 파일을 한 줄씩 읽음
      while (std::getline(trace_file, line)) {
        line_num++;

        // 빈 줄은 무시
        if (line.empty()) {
          continue;
        }

        // 공백 기준으로 토큰 분리
        std::vector<std::string> tokens;
        tokenize(tokens, line, " ");

        // 기대하는 trace 형식:
        // <t> <phase> <decode_step> <R/W> <hex_addr> <nbytes> <tensor>
        //
        // 예시:
        // 0 prefill -1 R 0x0000100000000000 64 model.layers.0.mlp.gate_proj.weight
        //
        // 실제로 이 frontend가 사용하는 정보는 딱 2개뿐임:
        //   tokens[3] = R/W 정보
        //   tokens[4] = 주소
        //
        // 나머지는 현재 코드에서는 무시:
        //   tokens[0] = 시간 또는 순번 t
        //   tokens[1] = phase (prefill/decode 등)
        //   tokens[2] = decode_step
        //   tokens[5] = 요청 크기(byte)
        //   tokens[6...] = tensor 이름
        if (tokens.size() < 6) {
          throw ConfigurationError(
            "Trace {} format invalid at line {}! Expected: <t> <phase> <decode_step> <R/W> <addr> <nbytes> [tensor...]",
            file_path_str, line_num
          );
        }

        // R/W 토큰 해석
        bool is_write = false;
        if (tokens[3] == "R") {
          is_write = false;
        } else if (tokens[3] == "W") {
          is_write = true;
        } else {
          // R이나 W가 아니면 잘못된 형식
          throw ConfigurationError(
            "Trace {} format invalid at line {}! R/W token must be R or W.",
            file_path_str, line_num
          );
        }

        // 주소 파싱
        // 16진수(0x...)이면 16진수로, 아니면 10진수로 해석
        Addr_t addr = -1;
        if (tokens[4].compare(0, 2, "0x") == 0 || tokens[4].compare(0, 2, "0X") == 0) {
          addr = std::stoull(tokens[4].substr(2), nullptr, 16);
        } else {
          addr = std::stoull(tokens[4], nullptr, 10);
        }

        // 파싱한 요청을 trace 벡터에 저장
        m_trace.push_back({is_write, addr});
      }

      trace_file.close();

      // 전체 trace 길이 저장
      m_trace_length = m_trace.size();
    };

    // 시뮬레이션 종료 조건
    // → 성공적으로 보낸 요청 수가 전체 trace 길이와 같아지면 종료
    bool is_finished() override {
      return m_trace_count >= m_trace_length;
    };
};

} // namespace Ramulator