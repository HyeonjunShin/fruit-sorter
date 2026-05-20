#include <iostream>
#include <cstring>

extern "C" {
    #include <soem/soem.h>
}

// --------------------------------------------------------------------------
// 1. ESI XML 기반 구조체 정의 (1바이트 정렬 제한)
// --------------------------------------------------------------------------
#pragma pack(push, 1)

// RxPDO (마스터 -> 서브디렉토리): 축당 24 바이트
struct AxisOutput {
    int32_t  target_position;  // PosX (DINT, 4B)
    int32_t  target_velocity;  // SpdX (DINT, 4B)
    uint32_t max_velocity;     // MaxSpdX (UDINT, 4B)
    uint32_t acceleration;     // AccX (UDINT, 4B)
    uint32_t deceleration;     // DecX (UDINT, 4B)
    uint16_t control_word;     // CWX (UINT, 2B)
    uint8_t  pattern;          // PatX (USINT, 1B)
    uint8_t  dummy;            // DummyX (USINT, 1B)
};

// 4개 축 Outputs (Sm 2번 -> DefaultSize="96")
struct SlaveOutputs {
    AxisOutput axis[4];
};

// TxPDO (서브디렉토리 -> 마스터): 축당 8 바이트
struct AxisInput {
    uint16_t status_word;      // SWX (UINT, 2B)
    int32_t  actual_position;  // ActPosX (DINT, 4B)
    uint16_t error_code;       // ErrX (UINT, 2B)
};

// 4개 축 Inputs (Sm 3번 -> DefaultSize="32")
struct SlaveInputs {
    AxisInput axis[4];
};
#pragma pack(pop)

// 실시간 데이터 교환을 위한 버퍼 공간 (Outputs: 96B + Inputs: 32B = 128B 소요)
uint8_t IOmap[4096] = {0};

int main(int argc, char *argv[]) {
    // 인자 확인
    if (argc < 2) {
        std::cout << "Usage: sudo ./main [interface_name]\n";
        return 1;
    }

    const char* ifname = argv[1];

    // 컨텍스트 생성 및 초기화
    ecx_contextt main_context;
    memset(&main_context, 0, sizeof(ecx_contextt));

    std::cout << "[EtherCAT] 인터페이스 초기화 시작: " << ifname << std::endl;

    if (ecx_init(&main_context, ifname) <= 0) {
        std::cerr << "❌ 네트워크 소켓 열기 실패. 인터페이스 이름을 확인하거나 sudo 권한을 쓰세요." << std::endl;
        std::cerr << "❌ ip link adadawd" << std::endl;
        return 1;
    }

    // 1. SubDevice 스캔 및 하드웨어 탐색 (자동으로 PRE_OP 유도)
    int dev_count = ecx_config_init(&main_context);

    if (dev_count > 0) {
        std::cout << "-> 총 " << dev_count << "개의 장치를 발견했습니다." << std::endl;
        std::cout << "----------------------------------------------------------" << std::endl;
        for (int i = 1; i <= dev_count; i++) {
            ecx_statecheck(&main_context, i, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
            printf("[%d] %s | State: 0x%02x\n", i, main_context.slavelist[i].name, main_context.slavelist[i].state);
        }
        std::cout << "----------------------------------------------------------" << std::endl;

        // 2. 강제 단계별 상태 천이 보정 (INIT -> PRE_OP 확인)
        std::cout << "[Step 1] 장치들을 명시적으로 PRE_OP 상태로 진입 확인 중..." << std::endl;
        main_context.slavelist[0].state = EC_STATE_PRE_OP;
        ecx_writestate(&main_context, 0);
        ecx_statecheck(&main_context, 0, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 4);

        if (main_context.slavelist[1].state != EC_STATE_PRE_OP) {
            std::cerr << "❌ 장치가 PRE_OP로 올라가지 않습니다. 현재 상태: 0x" 
                      << std::hex << (int)main_context.slavelist[1].state 
                      << " | AL StatusCode: 0x" << main_context.slavelist[1].ALstatuscode << std::dec << std::endl;
            ecx_close(&main_context);
            return 1;
        }
        std::cout << "-> PRE_OP 상태 진입 확인 완료." << std::endl;

        // 3. PDO 맵 구성 및 메모리 할당
        std::cout << "[Step 2] ESI 명세 기반 IOmap 매핑 및 할당..." << std::endl;
        int iomap_size = ecx_config_map_group(&main_context, IOmap, 0);
        std::cout << "-> SOEM 런타임 맵핑된 크기: " << iomap_size << " bytes" << std::endl;
        std::cout << "   (ESI 도면 기준: Outputs 96B + Inputs 32B = 총 128B)" << std::endl;

        // 4. Free Run 모드 적용을 위해 DC(분산 클록) 설정 생략
        std::cout << "[Step 3] Free Run 모드 적용 (DC 동기화 우회)" << std::endl;

        // 5. SAFE_OP 상태 전환 유도
        std::cout << "[Step 4] SAFE_OP 상태 전환 요청..." << std::endl;
        main_context.slavelist[0].state = EC_STATE_SAFE_OP;
        ecx_writestate(&main_context, 0);
        ecx_statecheck(&main_context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

        if (main_context.slavelist[1].state != EC_STATE_SAFE_OP) {
            std::cerr << "⚠️ SAFE_OP 전환 실패/지연. 현재 상태: 0x" 
                      << std::hex << (int)main_context.slavelist[1].state 
                      << " | AL StatusCode: 0x" << main_context.slavelist[1].ALstatuscode << std::dec << std::endl;
        } else {
            std::cout << "-> SAFE_OP 상태 진입 완료." << std::endl;
        }

        // 6. 워치독 인터럽트 오동작 방지 루프 및 OP 상태 전환 강제 요청
        std::cout << "[Step 5] 최고 제어 단계인 OP(Operational) 상태로 전환 시도..." << std::endl;
        main_context.slavelist[0].state = EC_STATE_OPERATIONAL;
        
        // OP 승인 직전 빈 PDO 프레임을 주입하여 STM32 하드웨어 워치독 트립을 원천 방지합니다.
        ecx_send_processdata(&main_context);
        ecx_receive_processdata(&main_context, EC_TIMEOUTRET);
        ecx_writestate(&main_context, 0);

        // 최대 2초 동안 끊임없이 패킷을 교환하며 슬레이브가 OP 단계로 골인할 때까지 대기
        int chk = 40;
        do {
            ecx_send_processdata(&main_context);
            ecx_receive_processdata(&main_context, EC_TIMEOUTRET);
            ecx_statecheck(&main_context, 0, EC_STATE_OPERATIONAL, 50000); // 50ms interval
        } while (chk-- && (main_context.slavelist[0].state != EC_STATE_OPERATIONAL));

        // 7. 결과 확인 및 실시간 통신 루프
        if (main_context.slavelist[0].state == EC_STATE_OPERATIONAL) {
            std::cout << "==========================================================" << std::endl;
            std::cout << "★ 대성공: Custom STM32 4축 제어기가 OP 상태에 도달했습니다!" << std::endl;
            std::cout << "==========================================================" << std::endl;
            std::cout << "5초간 실시간 4축 모터 데이터 입출력 제어를 수행합니다...\n" << std::endl;

            // SOEM 내부 버퍼 주소를 ESI 규격 구조체 포인터로 일대일 캐스팅 연결
            SlaveOutputs* cmd = reinterpret_cast<SlaveOutputs*>(main_context.slavelist[1].outputs);
            SlaveInputs*  feedback = reinterpret_cast<SlaveInputs*>(main_context.slavelist[1].inputs);

// 1ms 주기로 10000번 반복 (총 10초 구동)
            for (int loop = 0; loop < 1000000; loop++) {
                
                // ------------------------------------------------------------------
                // 💡 [사용자 요청] 1번 모터(axis[0]) 구동 파라미터 실시간 주입
                // ------------------------------------------------------------------
                // Pos1 (목표 위치): 테스트를 위해 현재는 0으로 두거나, 구동을 원하시면 값을 점진적으로 변경하세요.
                cmd->axis[0].target_position = 0;    
                
                // Spd1 = 1000
                cmd->axis[0].target_velocity = -1800;  
                
                // MaxSpd1 = 2000
                cmd->axis[0].max_velocity    = 2000;  
                
                // Acc1 = 1000, Dec1 = 1000
                cmd->axis[0].acceleration    = 1000;  
                cmd->axis[0].deceleration    = 1000;  
                
                // CW1 = 63 (제어 워드: 0x3F) -> 일반적으로 CiA402 프로필에서 
                // Ready to switch on(1) -> Switched on(3) -> Operation enabled(7) 단계를 거쳐 
                // 최종 구동 비트를 켜는 플래그로 많이 사용됩니다.
                cmd->axis[0].control_word    = 63;    
                
                // Pat1 = 0, Dummy1 = 0
                cmd->axis[0].pattern         = 0;     
                cmd->axis[0].dummy           = 0;     

                // ------------------------------------------------------------------
                // 나머지 2, 3, 4번 모터는 오동작 방지를 위해 안전하게 0으로 초기화
                // ------------------------------------------------------------------
                for (int j = 1; j < 4; j++) {
                    cmd->axis[j].target_position = 0;
                    cmd->axis[j].target_velocity = 0;
                    cmd->axis[j].max_velocity    = 0;
                    cmd->axis[j].acceleration    = 0;
                    cmd->axis[j].deceleration    = 0;
                    cmd->axis[j].control_word    = 0;
                    cmd->axis[j].pattern         = 0;
                    cmd->axis[j].dummy           = 0;
                }

                // ------------------------------------------------------------------
                // 2. 물리 라인으로 PDO 패킷 송수신 (이 타이밍에 데이터가 STM32로 전송됨)
                // ------------------------------------------------------------------
                ecx_send_processdata(&main_context);
                ecx_receive_processdata(&main_context, EC_TIMEOUTRET);

                // ------------------------------------------------------------------
                // 3. 500ms에 한 번씩 실제 STM32 보드가 보내주는 1번 모터 피드백 출력
                // ------------------------------------------------------------------
                if (loop % 500 == 0) {
                    std::cout << "[Loop " << loop << "] "
                              << "1번 모터 현재위치(ActPos1): " << feedback->axis[0].actual_position 
                              << " | 상태워드(StatusWord1): 0x" << std::hex << feedback->axis[0].status_word 
                              << " | 에러코드: 0x" << feedback->axis[0].error_code << std::dec << std::endl;
                }
                osal_usleep(1000); // 1ms Free Run 데이터 전송 타임
            }
        } else {
            std::cerr << "\n❌ 최종 OP 상태 진입 거부됨." << std::endl;
            ecx_statecheck(&main_context, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);
            printf("최종 Slave 1 링킹 상태: 0x%02x\n", main_context.slavelist[1].state);
            printf("AL Status Code (장치 오류 원인 스펙): 0x%04x\n", main_context.slavelist[1].ALstatuscode);
            std::cerr << "-> 만약 에러코드가 0x0000이고 0x01에 멈춰있다면 STM32 하드웨어 SPI 통신 라인 선로 및 인라인 인터럽트 구동을 체크하세요.\n";
        }

        // 8. 통신 탈출 안전 조치 (장치를 다시 초기 상태로 복구)
        std::cout << "\n[Step 6] 제어 루프 종료. 장치 상태를 INIT으로 다운그레이드..." << std::endl;
        main_context.slavelist[0].state = EC_STATE_INIT;
        ecx_writestate(&main_context, 0);
        ecx_statecheck(&main_context, 0, EC_STATE_INIT, EC_TIMEOUTSTATE);
    } else {
        std::cout << "❌ 네트워크 상에 활성화된 슬레이브 모듈이 전무합니다." << std::endl;
    }

    // 소켓 리소스 반환 및 종료
    ecx_close(&main_context);
    std::cout << "[EtherCAT] 통신 세션이 완전하게 회수되었습니다." << std::endl;
    return 0;
}