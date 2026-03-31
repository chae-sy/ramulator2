#ifndef RAMULATOR_CONTROLLER_CONTROLLER_H
#define RAMULATOR_CONTROLLER_CONTROLLER_H

#include <vector>
#include <deque>

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "base/base.h"
#include "dram/dram.h"
#include "dram_controller/scheduler.h" // request 중 어떤 걸 먼저 낼지 고르는 scheduler 인터페이스
#include "dram_controller/plugin.h" // controller plugin
#include "dram_controller/refresh.h" // refresh manager
#include "dram_controller/rowpolicy.h" // row open/close 정책


namespace Ramulator {
// interface of DRAM controller
// clock을 따라 움직이는 component. 매 cycle 마다 tick()이 호출되는 구조와 연결
class IDRAMController : public Clocked<IDRAMController> {  
  // interface 등록. RAMULATOR_REGISTER_INTERFACE(인터페이스 이름, 인터페이스 타입 문자열, 인터페이스 설명 문자열)
  RAMULATOR_REGISTER_INTERFACE(IDRAMController, "Controller", "Memory Controller Interface");

  public:
    IDRAM*  m_dram = nullptr; // 실제 DRAM device/model을 가리키는 포인터. 
                              // controller가 혼자 command를 처리하는 게 아니라 실제 DRAM state/timing을 물어봄. 
                              //  (row hit인지? ready인지? command issue 가능한지? read Latency 얼마인지? 등)         
    IScheduler*   m_scheduler = nullptr; // scheduler 정책
                                        // request buffer 안에 요청이 여러개 있을 때 이번 cycle에 어떤 요청 선택할지 결정
                                        // e.g., FCFS, FR-FCFS, row-hit 우선, oldest-first
    IRefreshManager*   m_refresh = nullptr; // refresh 시점 추적, refresh request 삽입, refresh 관련 state update
    IRowPolicy*   m_rowpolicy = nullptr; // row를 열어둔 채로 유지 or 빨리 닫을지 (e.g., open-page policy, close-page policy, adaptive policy)
    std::vector<IControllerPlugin*> m_plugins; // plug-in 추가 기능 모듈들

    int m_channel_id = -1; // controller가 담당하는 channel id. multi-channel DRAM에서 controller가 어느 channel 담당하는지 알려주는 정보
  public:
    /**
     * @brief       Send a request to the memory controller.
     * 
     * @param    req        The request to be enqueued.
     * @return   true       Successful.
     * @return   false      Failed (e.g., buffer full).
     */
    virtual bool send(Request& req) = 0; // request를 memory controller queue에 보내는 함수 

    /**
     * @brief       Send a high-priority request to the memory controller.
     * 
     */
    virtual bool priority_send(Request& req) = 0; // 일반 send()보다 우선순위 높은 요청을 넣는 함수. e.g., refresh, maintenance, special control request
                                                  // priority buffer 에 넣거나 특별한 queue에 넣는 식

    /**
     * @brief       Ticks the memory controller.
     * 
     */
    virtual void tick() = 0; // controller를 한 cycle 진행시키는 함수
   
};

}       // namespace Ramulator

#endif  // RAMULATOR_CONTROLLER_CONTROLLER_H