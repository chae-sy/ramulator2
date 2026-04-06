#include <filesystem>
#include <iostream>
#include <fstream>

#include "frontend/frontend.h"
#include "base/exception.h"

namespace Ramulator {
  // 이 frontend는 거의 “패스스루(pass-through)” 역할이야:
// GEM5 → (이 클래스) → MemorySystem

//👉 중간에서:

//요청 생성 ❌
//요청 변형 ❌
//스케줄링 ❌

//👉 그냥 그대로 전달만 함
// GEM5와 연동하기 위한 Frontend 구현
// → GEM5에서 들어온 메모리 요청을 Ramulator MemorySystem으로 전달하는 역할
class GEM5 : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, GEM5, "GEM5", "GEM5 frontend.")

  public:
    // 초기화 함수 (현재는 아무 작업도 안 함)
    void init() override { };

    // 매 cycle마다 호출되는 함수 (현재는 동작 없음)
    void tick() override { };

    // 외부(GEM5)에서 들어오는 메모리 요청을 처리하는 함수
    bool receive_external_requests(
        int req_type_id,   // 요청 타입 (READ / WRITE 등)
        Addr_t addr,       // 접근할 메모리 주소
        int source_id,     // 요청을 보낸 코어/소스 ID
        std::function<void(Request&)> callback // 요청 완료 시 실행할 콜백 함수
    ) override {

      // 핵심: 요청을 그대로 MemorySystem으로 전달
      // → 내부적으로 queue에 넣거나, 처리 가능 여부 판단 후 true/false 반환
      return m_memory_system->send({
          addr,          // 메모리 주소
          req_type_id,   // 요청 타입
          source_id,     // 요청 출처
          callback       // 완료 시 실행할 함수
      });
    }

  private:
    // GEM5 frontend는 종료 조건을 따로 관리하지 않음
    // → 항상 true를 반환해서 frontend 자체는 끝났다고 간주
    bool is_finished() override { return true; };
};

}        // namespace Ramulator