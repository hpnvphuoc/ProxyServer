//ProxyServer.cpp : Entry point của chương trình
//

#include "stdafx.h"
#include "ProxyServer.h"
#include "afxsock.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdlib>

using namespace std;

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define HTTPPORT 80
#define PROXYPORT 8888
#define BUFFERSIZE 204800
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
	char url[256];  //URL để caching
	HANDLE isReceived;  //Sự kiện (có thể gọi là cờ lệnh, dấu hiệu,...) đã nhận được gói response để caching
};

//Cấu trúc lưu trữ một gói tin
struct Cache
{
	char url[256];
	char data[BUFFERSIZE];
	int dataSize;
};

SOCKET TCPListen;  //Socket lắng nghe kết nối
char blacklist[BUFFERSIZE];  //Danh sách các host bị chặn
vector<Cache> cacheList;  //Cache

//Hàm khởi động proxy server
int StartProxy();

//Hàm kết thúc proxy server
void StopProxy();

//Thread truyền data lên server
UINT Upstream(LPVOID arg);

//Thread truyền data xuống client
UINT Downstream(LPVOID arg);

//Hàm lấy host trong request
int GetHost(char *buffer, char *host, char *url);

//Hàm lấy thông tin server
hostent* GetServerInfo(char *host);

//Hàm tải blacklist.conf
void LoadBlacklist(char *blacklist);

//Hàm kiểm tra blacklist
bool isForbidden(char *blacklist, char *host);

// Hàm kiểm tra url đã có trong cache hay chưa 
int isInCache(char *url);

using namespace std;

int main()
{
	int nRetCode = 0;

	HMODULE hModule = ::GetModuleHandle(nullptr);

	if (hModule != nullptr)
	{
		//Khởi tạo thư viện MFC
		if (!AfxWinInit(hModule, nullptr, ::GetCommandLine(), 0))
		{
			//Khởi tạo thất bại, thoát
			cerr << "Fatal Error: MFC initialization failed\n";
			nRetCode = 1;
		}
		else
		{
			//Khởi tạo thành công, bắt đầu chương trình
			StartProxy();
			while (1)
			{
				if (GetAsyncKeyState(VK_ESCAPE))
				{
					cout << "ProxyServer stopped." << endl;
					StopProxy();
					break;
				}
			}
		}
	}
	else
	{
		//Lỗi handle
		cerr << "Fatal Error: GetModuleHandle failed\n";
		nRetCode = 1;
	}
	system("Pause");
	return nRetCode;
}

int StartProxy()
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
	{
		//Khởi tạo WSA thất bại, thoát
		cerr << "WSAStartup() failed\n";
		WSACleanup();
		return -1;
	}

	TCPListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (TCPListen == INVALID_SOCKET)
	{
		//Tạo socket thất bại, thoát
		cerr << "socket() failed\n";
		WSACleanup();
		return -1;
	}

	sockaddr_in proxyAddr;
	memset(&proxyAddr, 0, sizeof(proxyAddr));
	proxyAddr.sin_family = AF_INET;  			//Kiểu IPv4
	proxyAddr.sin_port = htons(PROXYPORT);  	//Port 8888
	proxyAddr.sin_addr.s_addr = INADDR_ANY;  	//Gán mọi interface của máy cho socket
	if (bind(TCPListen, (sockaddr*)&proxyAddr, sizeof(proxyAddr)) != 0)//gan dia chi vo socket
	{
		//Gán địa chỉ cho socket thất bại, thoát
		cerr << "bind() failed\n";
		WSACleanup();
		return -1;
	}

	if (listen(TCPListen, 5) != 0)
	{
		//Lắng nghe thất bại, thoát
		cerr << "listen() failed\n";
		WSACleanup();
		return -1;
	}

	cerr << "ProxyServer started successful. Listening on port 8888...\n";
	cerr << "Press ESC to stop ProxyServer!\n";
	LoadBlacklist(blacklist);
	AfxBeginThread((AFX_THREADPROC)Upstream, nullptr);
	return 0;
}

void StopProxy()
{
	closesocket(TCPListen);
	WSACleanup();
}

UINT Upstream(LPVOID arg)
{
	sockaddr_in clientAddr;
	int addrLen = sizeof(clientAddr);
	SOCKET clientSide;
	clientSide = accept(TCPListen, (sockaddr*)&clientAddr, &addrLen);  //Chấp nhận một kết nối
	AfxBeginThread((AFX_THREADPROC)Upstream, nullptr);  //Gọi thread khác để phục vụ đồng thời các client khác

	if (clientSide != INVALID_SOCKET)
	{
		//Client kết nối thành công, mở 1/2 đường hầm proxy bên client
		ProxyTunnel tunnel;
		tunnel.clientSide = clientSide;
		tunnel.isClientSideOpening = true;
		tunnel.isServerSideOpening = false;

		//Nhận request đầu tiên từ client
		char buffer[BUFFERSIZE];
		int dataLen;

		dataLen = recv(clientSide, buffer, BUFFERSIZE, 0);
		if (dataLen < 1)
		{
			//Nhận request thất bại, đóng kết nối phía client
			if (tunnel.isClientSideOpening == true)
			{
				closesocket(tunnel.clientSide);
				tunnel.isClientSideOpening = false;
			}
			return -1;
		}

		//Lấy host để gọi thread kết nối tới server
		dataLen -= GetHost(buffer, tunnel.host, tunnel.url);

		if (isForbidden(blacklist, tunnel.host) == true)
		{
			//Bị chặn, trả về 403, thoát, không gửi gì cho server
			send(tunnel.clientSide, forbidden, strlen(forbidden), 0);
			if (tunnel.isClientSideOpening == true)
			{
				closesocket(tunnel.clientSide);
				tunnel.isClientSideOpening = false;
			}
			return -1;
		}

		tunnel.shakingDone = CreateEvent(nullptr, TRUE, FALSE, nullptr);  //Sự kiện (như đã mô tả ở trên)
		CWinThread *pThread = AfxBeginThread((AFX_THREADPROC)Downstream, (LPVOID)&tunnel);  //Gọi thread Downstream, bắt tay với server
		WaitForSingleObject(tunnel.shakingDone, 60000);  //Đợi dấu hiệu shaking xong
		CloseHandle(tunnel.shakingDone);

		bool isCacheSent = false;  //Cờ báo hiệu cache đã được gửi xuống client, không cần gửi request lên server

		int positionInCache = isInCache(tunnel.url);  //Tìm trang web trong cache
		if (positionInCache > 0)
		{
			//Có trang web này trong cache, lấy ra và gửi xuống cho client
			dataLen = send(tunnel.clientSide, cacheList[positionInCache - 1].data, cacheList[positionInCache - 1].dataSize, 0);
			isCacheSent = true;
		}

		//Truyền/nhận request khi cả 2 đầu đường hầm proxy đều mở
		while (tunnel.isClientSideOpening == true && tunnel.isServerSideOpening == true)
		{
			if (isCacheSent == false)
			{
				//Trang web yêu cầu chưa có trong cache, truyền request vừa nhận lên server
				dataLen = send(tunnel.serverSide, buffer, dataLen, 0);
				if (dataLen < 1)
				{
					//Truyền request thất bại, đóng kết nối phía server
					if (tunnel.isServerSideOpening == true)
					{
						closesocket(tunnel.serverSide);
						tunnel.isServerSideOpening = false;
					}
					continue;
				}

				tunnel.isReceived = CreateEvent(nullptr, TRUE, FALSE, nullptr);
				WaitForSingleObject(tunnel.isReceived, 60000);  //Đợi Downstream nhận và lưu gói tin response vào cache
				CloseHandle(tunnel.isReceived);
			}

			isCacheSent = false;
			//Tiếp tục nhận request từ client
			dataLen = recv(tunnel.clientSide, buffer, BUFFERSIZE, 0);
			if (dataLen < 1)
			{
				//Nhận request thất bại, đóng kết nối phía client
				if (tunnel.isClientSideOpening == true)
				{
					closesocket(tunnel.clientSide);
					tunnel.isClientSideOpening = false;
				}
				continue;
			}

			dataLen -= GetHost(buffer, tunnel.host, tunnel.url);
			int positionInCache = isInCache(tunnel.url);
			if (positionInCache > 0)
			{
				dataLen = send(tunnel.clientSide, cacheList[positionInCache - 1].data, cacheList[positionInCache - 1].dataSize, 0);
				isCacheSent = true;
			}
		}

		//Hoàn tất, đóng tất cả kết nối
		if (tunnel.isServerSideOpening == true)
		{
			closesocket(tunnel.serverSide);
			tunnel.isServerSideOpening = false;
		}
		if (tunnel.isClientSideOpening == true)
		{
			closesocket(tunnel.clientSide);
			tunnel.isClientSideOpening = false;
		}
		WaitForSingleObject(pThread->m_hThread, 30000);  //Đợi thread downstream kết thúc
	}
	return 0;
}

UINT Downstream(LPVOID arg)
{
	//Nhận danh tính của đường hầm proxy
	ProxyTunnel *tunnel = (ProxyTunnel*)arg;

	SOCKET serverSide;
	serverSide = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (serverSide == INVALID_SOCKET)
	{
		//Tạo socket thất bại
		SetEvent(tunnel->shakingDone);
		return -1;
	}

	hostent *hent = GetServerInfo(tunnel->host);
	if (hent == nullptr)
	{
		//Phân giải host thất bại
		SetEvent(tunnel->shakingDone);
		return -1;
	}

	sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = hent->h_addrtype;
	serverAddr.sin_port = htons(HTTPPORT);
	memcpy(&(serverAddr.sin_addr), hent->h_addr, hent->h_length);
	if (connect(serverSide, (sockaddr*)&serverAddr, sizeof(serverAddr)) != 0)
	{
		//Kết nối thất bại
		SetEvent(tunnel->shakingDone);
		return -1;
	}

	//Thành công, mở 1/2 đường hầm proxy phía server
	tunnel->serverSide = serverSide;
	tunnel->isServerSideOpening = true;
	SetEvent(tunnel->shakingDone);

	//Truyền/nhận response khi cả 2 đầu đường hầm proxy đều mở
	char buffer[BUFFERSIZE];
	int dataLen;
	while (tunnel->isClientSideOpening == true && tunnel->isServerSideOpening == true)
	{
		//Nhận response từ server
		dataLen = recv(tunnel->serverSide, buffer, BUFFERSIZE, 0);
		if (dataLen < 1)
		{
			//Nhận response thất bại, đóng kết nối phía server
			if (tunnel->isServerSideOpening == true)
			{
				closesocket(tunnel->serverSide);
				tunnel->isServerSideOpening = false;
			}
			SetEvent(tunnel->isReceived);
			continue;
		}

		//Lưu gói tin response này vào cache
		if (strstr(buffer, "text/html") != nullptr)
		{
			Cache temp;
			strcpy(temp.data, buffer);
			temp.dataSize = dataLen;
			strcpy(temp.url, tunnel->url);
			cacheList.push_back(temp);
			SetEvent(tunnel->isReceived);
		}

		//Truyền response xuống client
		dataLen = send(tunnel->clientSide, buffer, dataLen, 0);
		if (dataLen < 1)
		{
			//Truyền response thất bại, đóng kết nối phía client
			if (tunnel->isClientSideOpening == true)
			{
				closesocket(tunnel->clientSide);
				tunnel->isClientSideOpening = false;
			}
			continue;
		}
	}
	//Hoàn tất, đóng tất cả kết nối
	if (tunnel->isClientSideOpening == true)
	{
		closesocket(tunnel->clientSide);
		tunnel->isClientSideOpening = false;
	}
	if (tunnel->isServerSideOpening == true)
	{
		closesocket(tunnel->serverSide);
		tunnel->isServerSideOpening = false;
	}
	return 0;
}

int GetHost(char *buffer, char *host, char *url)
{
	char method[128], tempURL[BUFFERSIZE], protocol[512], *p = nullptr, *q = nullptr;

	sscanf(buffer, "%s%s%s", method, tempURL, protocol);
	//Tách phần url
	strcpy(url, tempURL);
	//Lấy phần host
	p = strstr(tempURL, "http://");
	if (p != nullptr)
	{
		p += 7;
		for (int i = 0; i < (int)strlen(p); i++)
			if (*(p + i) == '/')
			{
				*(p + i) = 0;
				break;
			}
		strcpy(host, p);
	}

	//Thêm/sửa trường Accept-Encoding thành identity
	q = strstr(buffer, "Accept-Encoding: ");
	if (q != nullptr)
	{
		q += 17;
		int numDelete = 0;
		for (numDelete = 0; *(q + numDelete + 2) != 'A'; numDelete++);

		if (numDelete > 8)
		{
			char* t = (p + numDelete);
			q[0] = 'i', q[1] = 'd', q[2] = 'e', q[3] = 'n', q[4] = 't', q[5] = 'i', q[6] = 't', q[7] = 'y';
			q += 8;
			for (int i = 0; i < (int)strlen(buffer) - (numDelete - 8); i++)
			{
				q[i] = q[i + numDelete - 8];
			}
			q[strlen(buffer) - (numDelete - 8)] = 0;
			return (numDelete - 8);
		}
	}
	return 0;
}

hostent* GetServerInfo(char *host)
{
	if (isalpha(host[0]))  //host là một cannonial name
		return gethostbyname(host);

	//host là một địa chỉ IP
	int addr = inet_addr(host);
	return gethostbyaddr((char*)&addr, 4, AF_INET);
}

void LoadBlacklist(char *blacklist)
{
	//Đọc các dòng host trong file blacklist.conf
	ifstream f;
	f.open("blacklist.conf", ios::in);
	while (f.eof() == false)
	{
		char host[256];
		f.getline(host, 256);
		blacklist = strcat(blacklist, host);
	}
	f.close();
}

bool isForbidden(char *blacklist, char *host)
{
	//Kiểm tra host có nằm trong blacklist không
	if (strstr(blacklist, host) == nullptr)
		return false;
	return true;
}

int isInCache(char *url)
{
	//Tìm xem trang web này đã được cache hay chưa
	if (cacheList.empty()) 
		return -1;
	for (int i = 0; i < (int)cacheList.size(); i++)
	{
		if (strstr(cacheList[i].url, url) != nullptr)
		{
			//Nếu có trang web này trong cache, trả về vị trí của nó
			return i + 1;
		}
	}
	return -1;
}