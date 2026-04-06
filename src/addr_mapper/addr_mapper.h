#ifndef RAMULATOR_ADDR_MAPPER_ADDR_MAPPER_H
#define RAMULATOR_ADDR_MAPPER_ADDR_MAPPER_H

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "base/base.h"
#include "dram_controller/controller.h"

namespace Ramulator {

// 주소 매핑 인터페이스
// → 물리 주소(Addr_t)를 DRAM 계층 구조에 맞는 주소 벡터(addr_vec)로 변환하는 역할
//
// 예:
//   물리 주소: 0x12345678
//   ↓
//   addr_vec: [channel, rank, bank, row, column]
//
// 즉, "어느 채널 / 어느 뱅크 / 어느 row / 어느 column인지"로 쪼개주는 모듈
class IAddrMapper {
  RAMULATOR_REGISTER_INTERFACE(
    IAddrMapper,
    "AddrMapper",
    "Memory Controller Address Mapper"
  );

  public:
    /**
     * @brief 물리 주소를 DRAM 주소 벡터로 변환하는 함수
     * 
     * @param req Request 객체 (req.addr에 물리 주소가 들어 있음)
     * 
     * 동작:
     * - req.addr (물리 주소)를 읽어서
     * - DRAM 구조(channel / rank / bank / row / column 등)에 맞게 분해하고
     * - 그 결과를 req.addr_vec에 저장
     * 
     * 예:
     *   입력:
     *     req.addr = 0x12345678
     * 
     *   출력:
     *     req.addr_vec = [ch=0, rank=1, bank=3, row=1024, col=8]
     * 
     * 이후 controller는 이 addr_vec을 사용해서
     * 어떤 DRAM 리소스를 사용할지 결정함
     */
    virtual void apply(Request& req) = 0;   
};

}       // namespace Ramulator

#endif  // RAMULATOR_ADDR_MAPPER_ADDR_MAPPER_H