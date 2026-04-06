#ifndef     RAMULATOR_MEMORYSYSTEM_MEMORY_H
#define     RAMULATOR_MEMORYSYSTEM_MEMORY_H

#include <map>
#include <vector>
#include <string>
#include <functional>

#include "base/base.h"
#include "frontend/frontend.h"

namespace Ramulator {

// MemorySystem 인터페이스 클래스
// → Frontend가 보낸 요청을 받아서
//   실제 메모리 컨트롤러/DRAM 계층으로 전달하고 처리하는 상위 인터페이스
//
// 쉽게 말하면:
// Frontend = 요청을 만들어서 넣는 쪽
// MemorySystem = 그 요청을 받아서 실제 메모리 동작으로 처리하는 쪽
class IMemorySystem : public TopLevel<IMemorySystem> {
  RAMULATOR_REGISTER_INTERFACE(
    IMemorySystem,
    "MemorySystem",
    "Memory system interface (e.g., communicates between processor and memory controller)."
  )

  friend class Factory;

  protected:
    IFrontEnd* m_frontend;   // 연결된 frontend 포인터
    uint m_clock_ratio = 1;  // Frontend와 MemorySystem 사이의 클럭 비율

  public:
    // Frontend를 MemorySystem에 연결하는 함수
    // → frontend와 memory system이 서로 연결될 때 호출됨
    virtual void connect_frontend(IFrontEnd* frontend) { 
      m_frontend = frontend; 

      // 현재 구현체(impl)에 대해 setup 수행
      m_impl->setup(frontend, this);

      // 하위 컴포넌트들도 모두 setup 수행
      for (auto component : m_components) {
        component->setup(frontend, this);
      }
    };

    // 시뮬레이션 종료 시 호출되는 함수
    // → 각 컴포넌트의 finalize를 호출하고,
    //   수집된 통계를 YAML 형식으로 출력함
    virtual void finalize() { 
      for (auto component : m_components) {
        component->finalize();
      }

      YAML::Emitter emitter;
      emitter << YAML::BeginMap;
      m_impl->print_stats(emitter);
      emitter << YAML::EndMap;

      std::cout << emitter.c_str() << std::endl;
    };

    /**
     * @brief 메모리 시스템으로 요청을 보내는 함수
     * 
     * @param req 보낼 요청(Request 객체)
     * 
     * @return true
     *   요청이 메모리 시스템에 정상적으로 받아들여짐
     *   예: 큐에 성공적으로 들어감
     * 
     * @return false
     *   요청이 거절됨
     *   예: 메모리 컨트롤러 큐가 가득 참
     *
     * 중요:
     * 이 함수는 "요청을 실제로 다 처리했다"는 뜻이 아니라,
     * 일단 메모리 시스템이 그 요청을 받아줄 수 있는지를 의미함
     */
    virtual bool send(Request req) = 0;

    /**
     * @brief 메모리 시스템을 한 tick 진행시키는 함수
     * 
     * 실제로는 이 안에서
     * - 큐에 들어온 요청을 스케줄링하고
     * - 필요한 DRAM command(ACT/READ/WRITE/PRE 등)를 발행하고
     * - 타이밍 제약을 검사하고
     * - 요청 완료 여부를 갱신하는 동작이 이루어질 수 있음
     */
    virtual void tick() = 0;

    /**
     * @brief Frontend와 MemorySystem 사이의 클럭 비율 반환
     * 
     * 예를 들어 frontend가 1번 tick할 때
     * memory system이 몇 번 tick하는지 등을 결정할 때 사용될 수 있음
     */
    int get_clock_ratio() { return m_clock_ratio; };

    // /**
    //  * @brief 메모리 스펙에서 지원하는 request 타입의 정수 ID를 반환하는 함수
    //  *
    //  * 현재는 주석 처리되어 있음
    //  */
    // virtual const SpecDef& get_supported_requests() = 0;

    // 메모리 클럭 주기(tCK)를 반환하는 함수
    // 기본 구현은 -1.0f를 반환하고,
    // 실제 메모리 시스템 구현체에서 override해서 사용 가능
    virtual float get_tCK() { return -1.0f; };
};

}        // namespace Ramulator

#endif   // RAMULATOR_MEMORYSYSTEM_MEMORY_H