#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <stdbool.h>

#pragma comment(lib, "ws2_32.lib") // Winsock 라이브러리 연결

#define PORT 8080
#define BUFFER_SIZE 512
#define WINDOW_SIZE 8
#define MAX_SEQ 10    // 최대 시퀀스 번호 정의

#pragma pack(push, 1)
typedef struct {
    int action;                 //시나리오 제어
    int packetId;               //패킷 아이디
    bool is_ack;                //윈도우 내부에서 ack 수신 확인
    char message[BUFFER_SIZE];  //데이터 부분, 있으나 없으나
} packetStruct;
#pragma pack(pop)

// 역직렬화 함수
void deserialize_packet(char *buffer, packetStruct *packet) {
    int offset = 0;

    // action (int) -> 호스트 바이트 순서
    int network_action;
    memcpy(&network_action, buffer + offset, sizeof(int));
    packet->action = ntohl(network_action);
    offset += sizeof(int);

    // packetId (int) -> 호스트 바이트 순서
    int network_packetId;
    memcpy(&network_packetId, buffer + offset, sizeof(int));
    packet->packetId = ntohl(network_packetId);
    offset += sizeof(int);

    // is_ack (bool) -> 단순 복사
    memcpy(&packet->is_ack, buffer + offset, sizeof(bool));
    offset += sizeof(bool);

    // message (char[]) -> 단순 복사
    memcpy(packet->message, buffer + offset, BUFFER_SIZE);
    offset += BUFFER_SIZE;
}

int handle_packet(SOCKET sockfd, char *buffer, size_t buffer_size, struct sockaddr_in *client_addr, 
int *client_len, int *lastpacketID, bool *first_input, packetStruct *window, bool *frist) {
    // 데이터 수신
    int n = recvfrom(sockfd, buffer, buffer_size, 0, (struct sockaddr *)client_addr, client_len);
    if (n == SOCKET_ERROR) {
        printf("Receive failed. Error Code : %d\n", WSAGetLastError());
        return 1; // 실패
    }
    packetStruct received_packet;
    // 역직렬화
    deserialize_packet(buffer, &received_packet);

    printf("----------> packet %d received\n", received_packet.packetId);//수신 출력 체크
    if (*frist && received_packet.action == 2 && received_packet.packetId == 7) {
        printf("Intentional packet %d ignoring **3-duk**\n\n", received_packet.packetId);
        *frist = FALSE;//해당 코드가 한번만 작동하도록함, 3-duk용
        return -1;//특정 조건 만족시 이후 행동 안하고 종료
    }
    if (*frist && received_packet.action == 3 && received_packet.packetId == 6) {
        printf("Intentional packet %d ignoring**time out**\n\n", received_packet.packetId);
        *frist = FALSE;//해당 코드가 한번만 작동하도록함, 타임아웃용
        Sleep(3000);
        return -1;//특정 조건 만족시 이후 행동 안하고 종료
    }

    // 수신된 패킷을 윈도우에 저장
    int seq_num = received_packet.packetId % WINDOW_SIZE;
    if (received_packet.packetId != -1) {
        window[seq_num] = received_packet;
    }

    return 0; // 성공
}

void selective_repeat(SOCKET sockfd, char *buffer, size_t buffer_size, 
struct sockaddr_in *client_addr, int *client_len, int *lastpacketID, bool *first_input) {
    int rcv_base = 0;  // 수신된 패킷의 기준 시퀀스 번호
    packetStruct window[WINDOW_SIZE];  // 윈도우 버퍼
    bool frist = TRUE;

    // 윈도우 버퍼 초기화
    for (int i = 0; i < WINDOW_SIZE; i++) {
        window[i].packetId = -1;  // 초기 상태는 패킷 아이디가 없도록 설정
    }

    while (1) {
        int seq_num = rcv_base % WINDOW_SIZE;  // 현재 윈도우의 시퀀스 번호

        // 패킷 처리
        if (handle_packet(sockfd, buffer, buffer_size, client_addr, client_len, 
        lastpacketID, first_input, window, &frist) != 0) {
            printf("3-duk or time out occurr\n");
            continue;
        }

        // 윈도우 내에서 처리된 패킷 확인
        while (window[rcv_base % WINDOW_SIZE].packetId == rcv_base) {
            // 순서대로 수신된 경우
            printf("Packet %d processed, updating rcv_base.\n", rcv_base);
            window[rcv_base % WINDOW_SIZE].packetId = -1;  // 패킷 제거
            rcv_base++;  // 윈도우 이동
        }

        // 누적 ACK 갱신
        *lastpacketID = rcv_base - 1;  // rcv_base 이전까지는 모두 수신됨
        snprintf(buffer, buffer_size, "%d", *lastpacketID);

        Sleep(100);
        // ACK 전송
        if (sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)client_addr, *client_len) == SOCKET_ERROR) {
            printf("Send failed. Error Code : %d\n", WSAGetLastError());
        } else {
            printf("<--- ACK %d sent\n", *lastpacketID);
        }

        // 부하 방지
        Sleep(500);
    }
}

int main() {
    WSADATA wsa;
    SOCKET sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[1024];
    int client_len, lastpacketID = 0;
    bool first_input = TRUE;

    // Winsock 초기화
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed. Error Code : %d\n", WSAGetLastError());
        return 1;
    }

    // 소켓 생성
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
        printf("Socket creation failed. Error Code : %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // 서버 주소 설정
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // 소켓 바인드
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Bind failed. Error Code : %d\n", WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        return 1;
    }
    printf("Receiver is ready and waiting for messages...\n");

    
    client_len = sizeof(client_addr);

    selective_repeat(sockfd, buffer, sizeof(packetStruct), (struct sockaddr_in *)&client_addr, &client_len, &lastpacketID, &first_input);    

    // 소켓 종료
    closesocket(sockfd);
    WSACleanup();
    printf("last return");
    return 0;
}
