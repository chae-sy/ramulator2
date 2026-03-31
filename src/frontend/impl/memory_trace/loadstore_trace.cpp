#include <filesystem>
#include <iostream>
#include <fstream>

#include "frontend/frontend.h"
#include "base/exception.h"

namespace Ramulator {

namespace fs = std::filesystem;

class LoadStoreTrace : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, LoadStoreTrace, "LoadStoreTrace", "Load/Store memory address trace.")

  private:
    struct Trace {
      bool is_write;
      Addr_t addr;
    };
    std::vector<Trace> m_trace;

    size_t m_trace_length = 0;
    size_t m_curr_trace_idx = 0;

    size_t m_trace_count = 0;

    Logger_t m_logger;

  public:
    void init() override {
      std::string trace_path_str = param<std::string>("path").desc("Path to the load store trace file.").required();
      m_clock_ratio = param<uint>("clock_ratio").required();

      m_logger = Logging::create_logger("LoadStoreTrace");
      m_logger->info("Loading trace file {} ...", trace_path_str);
      init_trace(trace_path_str);
      m_logger->info("Loaded {} lines.", m_trace.size());
    };

    void tick() override {
      if (m_trace_length == 0) {
        return;
      }

      const Trace& t = m_trace[m_curr_trace_idx];
      bool request_sent = m_memory_system->send(
        {t.addr, t.is_write ? Request::Type::Write : Request::Type::Read}
      );

      if (request_sent) {
        m_curr_trace_idx++;
        m_trace_count++;
      }
    };

  private:
    void init_trace(const std::string& file_path_str) {
      fs::path trace_path(file_path_str);
      if (!fs::exists(trace_path)) {
        throw ConfigurationError("Trace {} does not exist!", file_path_str);
      }

      std::ifstream trace_file(trace_path);
      if (!trace_file.is_open()) {
        throw ConfigurationError("Trace {} cannot be opened!", file_path_str);
      }

      std::string line;
      size_t line_num = 0;

      while (std::getline(trace_file, line)) {
        line_num++;

        if (line.empty()) {
          continue;
        }

        std::vector<std::string> tokens;
        tokenize(tokens, line, " ");

        // Expected flat trace format:
        // <t> <phase> <decode_step> <R/W> <hex_addr> <nbytes> <tensor>
        //
        // Example:
        // 0 prefill -1 R 0x0000100000000000 64 model.layers.0.mlp.gate_proj.weight
        //
        // We use only:
        //   tokens[3] = R/W
        //   tokens[4] = addr
        //
        // We ignore:
        //   tokens[0] = t
        //   tokens[1] = phase
        //   tokens[2] = decode_step
        //   tokens[5] = nbytes
        //   tokens[6...] = tensor name
        if (tokens.size() < 6) {
          throw ConfigurationError(
            "Trace {} format invalid at line {}! Expected: <t> <phase> <decode_step> <R/W> <addr> <nbytes> [tensor...]",
            file_path_str, line_num
          );
        }

        bool is_write = false;
        if (tokens[3] == "R") {
          is_write = false;
        } else if (tokens[3] == "W") {
          is_write = true;
        } else {
          throw ConfigurationError(
            "Trace {} format invalid at line {}! R/W token must be R or W.",
            file_path_str, line_num
          );
        }

        Addr_t addr = -1;
        if (tokens[4].compare(0, 2, "0x") == 0 || tokens[4].compare(0, 2, "0X") == 0) {
          addr = std::stoull(tokens[4].substr(2), nullptr, 16);
        } else {
          addr = std::stoull(tokens[4], nullptr, 10);
        }

        m_trace.push_back({is_write, addr});
      }

      trace_file.close();
      m_trace_length = m_trace.size();
    };

    bool is_finished() override {
      return m_trace_count >= m_trace_length;
    };
};

} // namespace Ramulator