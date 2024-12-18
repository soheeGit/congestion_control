#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <stdbool.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib") // Winsock 라이브러리 연결

#define SERVER_IP "127.0.0.1" // 수신 측 IP 주소
#define PORT 8080
#define BUFFER_SIZE 512
#define MAX_WINDOW_SIZE 15  // 윈도우 크기의 최대값
#define PACKET_COUNT 15

#pragma pack(push, 1)
typedef struct {
    int action;                 // 시나리오 제어
    int packetId;               // 패킷 아이디
    bool is_ack;                // 윈도우 내부에서 ack 수신 확인
    char message[BUFFER_SIZE];  // 데이터 부분
} packetStruct;
#pragma pack(pop)

// 스레드 파라미터 구조체
typedef struct {
    SOCKET sockfd;
    struct sockaddr_in server_addr;
    char *buffer;
    packetStruct *packetArray;
    int *window_size;
    float *threshold;
    int *accum_ack;
    DWORD *shared_timer; // 공유 타이머 추가
} threadParams;

// 직렬화 함수
void serialize_packet(packetStruct *packet, char *buffer) {
    int offset = 0;

    // action (int) -> 네트워크 바이트 순서
    int network_action = htonl(packet->action);
    memcpy(buffer + offset, &network_action, sizeof(int));
    offset += sizeof(int);

    // packetId (int) -> 네트워크 바이트 순서
    int network_packetId = htonl(packet->packetId);
    memcpy(buffer + offset, &network_packetId, sizeof(int));
    offset += sizeof(int);

    // is_ack (bool) -> 단순 복사 (bool은 1바이트로 가정)
    memcpy(buffer + offset, &packet->is_ack, sizeof(bool));
    offset += sizeof(bool);

    // message (char[]) -> 단순 복사
    memcpy(buffer + offset, packet->message, BUFFER_SIZE);
    offset += BUFFER_SIZE;
}

int send_packet(SOCKET sockfd, packetStruct *packet, struct sockaddr_in *server_addr, char *buffer) {
    serialize_packet(packet, buffer);

    if (sendto(sockfd, buffer, sizeof(packetStruct), 0, (struct sockaddr *)server_addr, sizeof(*server_addr)) == SOCKET_ERROR) {
        printf("Send failed. Error Code: %d\n", WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        return 1;
    }
    Sleep(100);

    printf("----------> packet %d send\n", packet->packetId);
    fflush(stdout);
    return 0;
}

int receive_ack(SOCKET sockfd, struct sockaddr_in *server_addr, char *buffer, packetStruct *packet) {
    int server_len = sizeof(*server_addr);
    int n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)server_addr, &server_len);

    if (n == SOCKET_ERROR) {
        printf("Receive failed. Error Code : %d\n", WSAGetLastError());
        return 1;
    }

    buffer[n] = '\0';

    int temp = atoi(buffer);
    if (temp < 0 || temp >= PACKET_COUNT) {
        printf("Invalid ACK received: %s\n", buffer);
        return -1;
    }
    
    packet[temp].is_ack = TRUE;
    if (temp > 0 && packet[temp - 1].is_ack != packet[temp].is_ack){
        for (int i = 0; i < temp; i++){
            packet[i].is_ack = TRUE;
        }
    }
        
    printf("<--- ACK %d received\n", temp);
    fflush(stdout);
    return temp;
}

void handle_3_dup_ack(int dup_ack, int *window_size, float *threshold, SOCKET sockfd, struct sockaddr_in *server_addr, packetStruct *packetArray, char *buffer) {
    printf("3-dup-ACK detected for packet %d\n", dup_ack);
    *threshold = *window_size / 2.0;
    *window_size = (int)(*threshold) + 3;
    printf("Threshold updated to %.2f, window size set to %d\n", *threshold, *window_size);

    // 패킷 재전송
    if (send_packet(sockfd, &packetArray[dup_ack + 1], server_addr, buffer) != 0) {
        printf("Retransmission failed for packet %d\n", dup_ack + 1);
    } else {
        printf("Packet %d retransmitted successfully.\n", dup_ack + 1);
    }
}

void handle_timeout(int *window_size, float *threshold) {
    printf("Timeout detected\n");
    *threshold = *window_size / 2.0;
    *window_size = 1;
    printf("Threshold updated to %.2f, window size reset to %d\n", *threshold, *window_size);
}

void detect_3duk(int ackID, int *lastID, bool *first_input, int *count, int *window_size, float *threshold, 
    SOCKET sockfd, struct sockaddr_in *server_addr, packetStruct *packetArray, char *buffer, DWORD *shared_timer) {
    if (*first_input) {
        *lastID = ackID;
        *first_input = FALSE;
        *count = 0;
    } else if (ackID > *lastID) {
        (*lastID) = ackID;
        *count = 0;
    } else {
        (*count)++;
        if (*count >= 3) {
            *count = 0;
            printf("\n***** 3-duplicate ACK *****\n\n");
            handle_3_dup_ack(*lastID, window_size, threshold, sockfd, server_addr, packetArray, buffer); // 3-duplicate ACK 처리
            *shared_timer = GetTickCount();
        }
    }
}

void time_reset(int ackID, int *lastID, bool *first_input, DWORD *shared_timer, packetStruct *packet) {
    if (*first_input) {
        *lastID = ackID;
        *first_input = FALSE;
        *shared_timer = GetTickCount();//첫 수신 ack는 시간 그냥 저장
    } else if (ackID >= *lastID + 1) {//이후 부터는 누적 ack을 적용 
        (*lastID) = ackID;
        *shared_timer = GetTickCount();
    } else{}//송신한 패킷ID보다 작으면 무시
}

// ACK 수신 스레드 함수
DWORD WINAPI recv_ack_thread_func(LPVOID lpParam) {
    int lastID1, lastID2, count = 0;
    bool first_input1 = TRUE, first_input2 = TRUE;
    //int lastSentTime = GetTickCount();
    threadParams *params = (threadParams *)lpParam;
    
    *(params->accum_ack) = receive_ack(params->sockfd, &params->server_addr, params->buffer, params->packetArray);
    time_reset(*(params->accum_ack), &lastID2, &first_input2, params->shared_timer, params->packetArray);
    while (1) {
        if (*(params->accum_ack) == -1) {
            // Handle error
        }
        Sleep(100);
        
        // 3-중복 ACK 체크
        detect_3duk(*(params->accum_ack), &lastID1, &first_input1, &count, params->window_size, params->threshold, 
            params->sockfd, &params->server_addr, params->packetArray, params->buffer, params->shared_timer);
        
        *(params->accum_ack) = receive_ack(params->sockfd, &params->server_addr, params->buffer, params->packetArray);
        time_reset(*(params->accum_ack), &lastID2, &first_input2, params->shared_timer, params->packetArray);
    }
    return 0;
}

int main() {
    WSADATA wsa;
    SOCKET sockfd;
    struct sockaddr_in server_addr;
    char buffer[1024];
    packetStruct packet[PACKET_COUNT];
    int number;
    float threshold = 8.0;  // threshold 값 설정
    int window_size = 1; // 초기 윈도우 크기
    int accum_ack = 0; //누적 ack접근
    DWORD shared_timer;

    printf("1. no loss 2. 3-duk 3. time out\nChoose a scenario: ");
    scanf("%d", &number);

    for (int i = 0; i < PACKET_COUNT; i++) {
        packet[i].action = number;
        packet[i].packetId = i;
        packet[i].is_ack = FALSE;
        snprintf(packet[i].message, BUFFER_SIZE, "Packet %d: This is a test message", i);
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed. Error Code : %d\n", WSAGetLastError());
        return 1;
    }

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
        printf("Socket creation failed. Error Code : %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    server_addr.sin_port = htons(PORT);

    threadParams params;
    params.sockfd = sockfd;
    params.server_addr = server_addr;
    params.buffer = buffer;
    params.packetArray = packet;
    params.window_size = &window_size;
    params.threshold = &threshold;
    params.accum_ack = &accum_ack;
    params.shared_timer = &shared_timer;

    HANDLE hThread = CreateThread(NULL, 0, recv_ack_thread_func, &params, 0, NULL);
    if (hThread == NULL) {
        printf("CreateThread failed (%d)\n", GetLastError());
        closesocket(sockfd);
        WSACleanup();
        return 1;
    }

    shared_timer = GetTickCount(); // 타이머 시작
    for (int i = 0; i < PACKET_COUNT;) {
        int window_end = i + window_size < PACKET_COUNT ? i + window_size : PACKET_COUNT;
        for (int j = i; j < window_end; j++) {
            if (!packet[j].is_ack) {//ack 못 받은거만 전송해라
                if (send_packet(sockfd, &packet[j], &server_addr, buffer) != 0) {
                printf("Error sending packet %d\n", packet[j].packetId);
                }
            }
        }
        //
        bool timeout_occurred = FALSE;

        while (1) {
            bool all_ack_received = TRUE;
            for (int j = i; j < window_end; j++) {
                if (!packet[j].is_ack) {
                    all_ack_received = FALSE;
                    break;
                }
            }

            if (all_ack_received) {
                if (window_size < threshold) {
                    window_size *= 2; // 윈도우 크기 2배 증가
                } else {
                    window_size += 1; // 선형 증가
                }
                i = window_end; // 다음 윈도우로 이동
                break;
            }

            // 타임아웃 처리: 4초 초과 시 종료
            if (GetTickCount() - shared_timer > 4000) {
                printf("Timeout occurred for packets %d to %d\n", i, window_end - 1);
                threshold = window_size / 2.0;
                window_size = 1; // 윈도우 크기 감소
                printf("Threshold updated to %.2f, window size reset to %d\n\n", threshold, window_size);
                timeout_occurred = TRUE;

                // 타임아웃된 패킷 재전송
                for (int j = i; j < window_end; j++) {
                    if (!packet[j].is_ack) { // ACK를 받지 못한 패킷만 재전송
                        if (send_packet(sockfd, &packet[j], &server_addr, buffer) != 0) {
                            printf("Error resending packet %d\n", packet[j].packetId);
                        }
                    }
                }
                break;            
            }

            Sleep(100);  // 잠시 대기
        }

        if (timeout_occurred) {
            // 누적 ACK에 따라 i를 갱신 (가장 마지막으로 ACK 받은 패킷 다음으로 이동)
            i = accum_ack + 1;
            timeout_occurred = FALSE;
            printf("The value of i has changed according to the accumulated ack\n");
        }
    }

    printf("Press Enter to exit...\n");
    getchar();
    getchar();

    TerminateThread(hThread, 0);
    CloseHandle(hThread);
    closesocket(sockfd);
    WSACleanup();
    return 0;
}
