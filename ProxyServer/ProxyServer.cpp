//ProxyServer.cpp : Entry point của chương trình
#include "ProxyServer.h"

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

			}
			StopProxy();
		}
	}
	else
	{
		//Lỗi handle
		cerr << "Fatal Error: GetModuleHandle failed\n";
		nRetCode = 1;
	}

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
	proxyAddr.sin_family = AF_INET;  //Kiểu IPv4
	proxyAddr.sin_port = htons(PROXYPORT);  //Port 8888
	proxyAddr.sin_addr.s_addr = INADDR_ANY;  //Gán mọi interface của máy cho socket
	if (bind(TCPListen, (sockaddr*)&proxyAddr, sizeof(proxyAddr)) != 0)
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

	cerr << "ProxyServer start success. Listening on port 8888...\n";
	LoadBlacklist(blacklist);
	AfxBeginThread((AFX_THREADPROC)Upstream, nullptr);
	return 0;
}

void StopProxy()
{
	closesocket(TCPListen);
	WSACleanup();
}

void LoadBlacklist(char* blacklist)
{
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

bool isForbidden(char* blacklist, char* host)
{
	if (strstr(blacklist, host) == nullptr)
		return false;
	return true;
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

		//Nhận gói data đầu tiên từ client
		char buffer[BUFFERSIZE];
		int dataLen;

		dataLen = recv(clientSide, buffer, BUFFERSIZE, 0);
		if (dataLen < 1)
		{
			//Nhận data thất bại, đóng kết nối phía client
			if (tunnel.isClientSideOpening == true)
			{
				closesocket(tunnel.clientSide);
				tunnel.isClientSideOpening = false;
			}
			return -1;
		}

		//Lấy host, gọi thread kết nối tới server
		dataLen -= GetHost(buffer, tunnel.host);
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
		CWinThread* pThread = AfxBeginThread((AFX_THREADPROC)Downstream, (LPVOID)&tunnel);  //Gọi thread Downstream, bắt tay với server
		WaitForSingleObject(tunnel.shakingDone, 60000);  //Đợi dấu hiệu shaking xong
		CloseHandle(tunnel.shakingDone);

		//Truyền/nhận data khi cả 2 đầu đường hầm proxy đều mở
		while (tunnel.isClientSideOpening == true && tunnel.isServerSideOpening == true)
		{
			//Truyền data vừa nhận lên server
			dataLen = send(tunnel.serverSide, buffer, dataLen, 0);
			if (dataLen < 1)
			{
				//Truyền data thất bại, đóng kết nối phía server
				if (tunnel.isServerSideOpening == true)
				{
					closesocket(tunnel.serverSide);
					tunnel.isServerSideOpening = false;
				}
				continue;
			}

			//Tiếp tục nhận data từ client
			dataLen = recv(tunnel.clientSide, buffer, BUFFERSIZE, 0);
			if (dataLen < 1)
			{
				//Nhận data thất bại, đóng kết nối phía client
				if (tunnel.isClientSideOpening == true)
				{
					closesocket(tunnel.clientSide);
					tunnel.isClientSideOpening = false;
				}
				continue;
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
	ProxyTunnel* tunnel = (ProxyTunnel*)arg;

	SOCKET serverSide;
	serverSide = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (serverSide == INVALID_SOCKET)
	{
		//Tạo socket thất bại
		SetEvent(tunnel->shakingDone);
		return -1;
	}

	hostent* hent = GetServerInfo(tunnel->host);
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

	//Truyền/nhận data khi cả 2 đầu đường hầm proxy đều mở
	char buffer[BUFFERSIZE];
	int dataLen;
	while (tunnel->isClientSideOpening == true && tunnel->isServerSideOpening == true)
	{
		//Nhận data từ server
		dataLen = recv(tunnel->serverSide, buffer, BUFFERSIZE, 0);
		if (dataLen < 1)
		{
			//Nhận data thất bại, đóng kết nối phía server
			if (tunnel->isServerSideOpening == true)
			{
				closesocket(tunnel->serverSide);
				tunnel->isServerSideOpening = false;
			}
			continue;
		}

		//Truyền data xuống client
		dataLen = send(tunnel->clientSide, buffer, dataLen, 0);
		if (dataLen < 1)
		{
			//Truyền data thất bại, đóng kết nối phía client
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

int GetHost(char* buffer, char* host)
{
	char method[128], url[BUFFERSIZE], protocol[512], * p = nullptr;
	//Tách phần url
	sscanf(buffer, "%s%s%s", method, url, protocol);
	//Lấy phần host
	p = strstr(url, "http://");
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
	return 0;
}

hostent* GetServerInfo(char* host)
{
	if (isalpha(host[0]))
		return gethostbyname(host);

	int addr = inet_addr(host);
	return gethostbyaddr((char*)&addr, 4, AF_INET);
}