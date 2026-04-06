#ifndef     RAMULATOR_FRONTEND_FRONTEND_H
#define     RAMULATOR_FRONTEND_FRONTEND_H

#include <vector>
#include <string>
#include <functional>

#include "base/base.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

// Frontend 인터페이스 클래스
// → 외부에서 들어오는 메모리 요청을 받아서 MemorySystem으로 전달하는 역할
class IFrontEnd : public Clocked<IFrontEnd>, public TopLevel<IFrontEnd> {
  RAMULATOR_REGISTER_INTERFACE(IFrontEnd, "Frontend", "The frontend that drives the simulation.");

  friend class Factory;

  protected:
    IMemorySystem* m_memory_system;  // 연결된 메모리 시스템 (DRAM 등)
    uint m_clock_ratio = 1;          // Frontend와 MemorySystem 간 클럭 비율

  public:
    // MemorySystem과 연결하는 함수
    // → frontend가 메모리 시스템에 요청을 보낼 수 있도록 설정
    virtual void connect_memory_system(IMemorySystem* memory_system) { 
      m_memory_system = memory_system; 

      // 구현체(setup) 초기화
      m_impl->setup(this, memory_system);

      // 하위 컴포넌트들도 모두 초기화
      for (auto component : m_components) {
        component->setup(this, memory_system);
      }
    };

    // 시뮬레이션이 끝났는지 여부를 반환하는 함수 (pure virtual)
    // → 각 frontend 구현에서 반드시 정의해야 함
    virtual bool is_finished() = 0;

    // 시뮬레이션 종료 시 호출되는 함수
    // → 통계 출력 및 정리 작업 수행
    virtual void finalize() { 
      // 각 컴포넌트 finalize 수행
      for (auto component : m_components) {
        component->finalize();
      }

      // YAML 형식으로 통계 출력
      YAML::Emitter emitter;
      emitter << YAML::BeginMap;
      m_impl->print_stats(emitter);
      emitter << YAML::EndMap;

      std::cout << emitter.c_str() << std::endl;
    };

    // 코어 개수 반환 (기본값: 1)
    virtual int get_num_cores() { return 1; };

    // 클럭 비율 반환
    int get_clock_ratio() { return m_clock_ratio; };

    /**
     * @brief 외부에서 들어오는 메모리 요청을 처리하는 함수
     * 
     * @details
     * - 예: GEM5 같은 full-system simulator에서 요청이 들어옴
     * - 이 요청을 Ramulator 내부 Request 형태로 변환
     * - MemorySystem으로 전달 시도
     * - 성공 여부를 반환
     * 
     * @param req_type_id 요청 타입 (read/write 등)
     * @param addr        메모리 주소
     * @param source_id   요청을 보낸 소스 (코어 등)
     * @param callback    요청 완료 시 호출할 함수
     * 
     * @return true  → 요청을 성공적으로 보냄
     * @return false → 큐가 가득 차서 실패 등
     */
    virtual bool receive_external_requests(
        int req_type_id,
        Addr_t addr,
        int source_id,
        std::function<void(Request&)> callback
    ) { return false; }
};

}        // namespace Ramulator

#endif   // RAMULATOR_FRONTEND_FRONTEND_H