#pragma once
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "afxsock.h"
#include <iostream>
#include <fstream>
using namespace std;

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define HTTPPORT 80
#define PROXYPORT 8888
#define BUFFERSIZE 40960
char forbidden[46] = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";

//Định danh của chương trình
CWinApp ProxyServer;

//Đường hầm proxy ảo cho kết nối của một cặp client - proxy
struct ProxyTunnel
{
	SOCKET clientSide;
	SOCKET serverSide;
	bool isClientSideOpening;
	bool isServerSideOpening;
	char host[256];  //Địa chỉ của server, có thể là domain name hoặc địa chỉ IP
	HANDLE shakingDone;  //Sự kiện (có thể gọi là cờ lệnh, dấu hiệu,...) đã bắt tay với server
};

SOCKET TCPListen;  //Socket lắng nghe kết nối
char blacklist[BUFFERSIZE];

//Hàm khởi động proxy server
int StartProxy();

//Hàm kết thúc proxy server
void StopProxy();

//Hàm tải blacklist.conf
void LoadBlacklist(char* blacklist);

//Hàm kiểm tra blacklist
bool isForbidden(char* blacklist, char* host);

//Thread truyền data lên server
UINT Upstream(LPVOID arg);

//Thread truyền data xuống client
UINT Downstream(LPVOID arg);

//Hàm lấy host trong request
int GetHost(char* buffer, char* host);

//Hàm lấy thông tin server
hostent* GetServerInfo(char* host);

