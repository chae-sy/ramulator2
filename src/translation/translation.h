#ifndef     RAMULATOR_FRONTEND_TRANSLATION_H
#define     RAMULATOR_FRONTEND_TRANSLATION_H

#include <vector>
#include <unordered_map>
#include <string>
#include <functional>

#include "base/base.h"
#include "base/request.h"

namespace Ramulator {

// 디버그 플래그 선언
// → 주소 변환 과정의 디버그 로그를 켜고 끌 때 사용할 수 있음
DECLARE_DEBUG_FLAG(DTRANSLATE);
// ENABLE_DEBUG_FLAG(DTRANSLATE);

// 주소 변환(translation) 인터페이스
// → Request 안에 들어 있는 주소를
//   가상 주소(VA)에서 물리 주소(PA)로 바꾸는 역할
class ITranslation {
  RAMULATOR_REGISTER_INTERFACE(
    ITranslation,
    "Translation",
    "Interface for translation virtual address to physical address."
  )   

  public:
    /**
     * @brief 요청 req에 대해 주소 변환을 수행하는 함수
     * 
     * 예를 들어:
     * - req.addr 가 가상 주소라면
     * - 이를 물리 주소로 바꿔서 req 안에 반영함
     * 
     * @param req 변환할 Request 객체
     * 
     * @return true
     *   주소 변환 성공
     * 
     * @return false
     *   주소 변환 실패
     */
    virtual bool translate(Request& req) = 0;

    /**
     * @brief 특정 목적(type)으로 addr 주소를 예약하는 함수
     * 
     * 예:
     * - 어떤 버퍼나 특정 메모리 영역을 미리 점유하고 싶을 때
     * - 또는 특정 타입의 데이터에 대해 주소 공간을 따로 할당하고 싶을 때
     * 
     * 기본 구현은 false를 반환하며,
     * 실제 translation 구현체에서 필요하면 override해서 사용함
     * 
     * @param type 예약 목적을 나타내는 문자열
     * @param addr 예약할 주소
     * 
     * @return true  예약 성공
     * @return false 예약 실패
     */
    virtual bool reserve(const std::string& type, Addr_t addr) {
      return false;
    };

    /**
     * @brief 현재 translation이 지원하는 최대 물리 주소를 반환하는 함수
     * 
     * 예:
     * - 물리 주소 공간의 상한선을 알고 싶을 때 사용 가능
     * 
     * 기본 구현은 0을 반환하며,
     * 실제 구현체에서 override할 수 있음
     * 
     * @return 최대 물리 주소
     */
    virtual Addr_t get_max_addr() {
      return 0;
    };

};

}        // namespace Ramulator

#endif   // RAMULATOR_FRONTEND_TRANSLATION_H