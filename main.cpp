/**
*@desc:   crash监控服务器程序
*@author: edwards
*@date:   2018.07.23
*/

#include <iostream>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>  //for htonl() and htons()
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <signal.h>     //for signal()
#include <pthread.h>
#include <semaphore.h>
#include <list>
#include <map>
#include <set>
#include <errno.h>
#include <time.h>
#include <sstream>
#include <iomanip> //for std::setw()/setfill()
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>
#include <assert.h>

#define PROTO_HEADER_START   0xCD
#define WORKER_THREAD_NUM   1
#define min(a, b) ((a <= b) ? (a) : (b)) 
#define ETRYAGAIN(x)        (x==EAGAIN||x==EWOULDBLOCK||x==EINTR)  
const uint16_t kProtoPacketMaxLen = 23;//bytes

#pragma pack(1)
enum	CLIENTSTATE
{
	CONNECTING,//正在连接
	WAITHEARTBEAT,//等待心跳，单向的
	TIMEOUT//已断开
};

typedef struct{

	timeval		timestamp;//指令时间戳
	int			obj_process_pid;//目标进程的pid
	bool        connect_state;//目标连接状态

}tcprecvpacket_t;


typedef struct{

	uint8_t		ip[4];
	uint16_t	port;
	uint64_t	sTime;
	uint32_t	pid;
	//uint8_t port[2];
	//uint8_t sTime[8];
	//uint8_t pid[4];

}HeaderFlag_t;


typedef struct{

	uint8_t			start_byte;
	uint16_t		len;
	HeaderFlag_t	flag;

}PacketHeader_t;

typedef struct{

	uint16_t appID;

}PacketFileds_t;

typedef struct{

}PacketParity_t;

typedef struct{

	PacketHeader_t header;
	PacketFileds_t fileds;
	PacketParity_t parity;

}DeamonHeartPacket_t;

typedef union{

	DeamonHeartPacket_t packet;
	uint8_t				fragment[kProtoPacketMaxLen];

}DeamonExternalMonitorProto_t;

typedef struct{

	CLIENTSTATE		lastState;//目标连接状态
	int64_t			lastRecvTimestamp;
	HeaderFlag_t	flag;

}ClientInfo_t;


#pragma pack()



namespace daemonHeart
{
	namespace detail
	{

		int createEventfd()
		{
			int evtfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
			if (evtfd < 0)
			{
				printf("Failed in eventfd");
				abort();
			}
			return evtfd;
		}

		//读取事件，如果不读取由于Epoll水平触发会导致一直有IO事件产生
		void readEventfd(int eventfd)
		{
			uint64_t one = 1;
			ssize_t n = read(eventfd, &one, sizeof one);
		}

		int createTimerfd()
		{
			/*
			与CLOCK_REALTIME相反，它是以绝对时间为准，获取的时间为系统最近一次重启到现在的时间，
			更该系统时间对其没影响。
			*/
			int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
			if (timerfd < 0)
			{
				printf("Failed in timerfd_create");
			}
			return timerfd;
		}

		//读取事件，如果不读取由于Epoll水平触发会导致一直有IO事件产生
		void readTimerfd(int timerfd)
		{
			uint64_t howmany;
			ssize_t n = read(timerfd, &howmany, sizeof(howmany));
		}

		//暂支持精度为秒的周期定时
		//timeSeconds == 0即停止定时器
		void resetTimerfd(int timerfd, int delaySeconds, int intervalSeconds, bool stopFlag)
		{
			if (timerfd == 0)return;
			// wake up loop by timerfd_settime()
			struct itimerspec newValue;
			struct itimerspec oldValue;
			bzero(&newValue, sizeof newValue);
			bzero(&oldValue, sizeof oldValue);

			if (!stopFlag)
			{
				newValue.it_value.tv_sec = delaySeconds;//设置第一次，则延时delaySeconds启动
				newValue.it_value.tv_nsec = 1000000;
				newValue.it_interval.tv_sec = intervalSeconds;//周期间隔
				newValue.it_interval.tv_nsec = 0;
			}

			//it_interval不为0则表示是周期性定时器。
			//it_value和it_interval都为0表示停止定时器。
			//假如it_value为0是不会触发信号的，所以要能触发信号，it_value得大于0；如果it_interval为零，只会延时，不会定时（也就是说只会触发一次信号)。

			/*
			该函数的功能为启动和停止定时器。
			第一个参数fd为上面的timerfd_create()函数返回的定时器文件描述符，
			第二个参数flags为0表示相对定时器，为TFD_TIMER_ABSTIME表示绝对定时器，
			第三个参数new_value用来设置超时时间，为0表示停止定时器，
			第四个参数为原来的超时时间，一般设为NULL。

			需要注意的是我们可以通过clock_gettime获取当前时间，
			如果是绝对定时器，那么我们得获取1970.1.1到当前时间(CLOCK_REALTIME),再加上我们自己定的定时时间。
			若是相对定时，则要获取我们系统本次开机到目前的时间加我们要定的时常(即获取CLOCK_MONOTONIC时间)

			struct itimerspec {
			struct timespec it_interval;  // Interval for periodic timer
			struct timespec it_value;     // Initial expiration
			};

			*/
			int ret = timerfd_settime(timerfd, 0, &newValue, &oldValue);
			if (ret)
			{
				printf("timerfd_settime() Err: %d", errno);
			}
		}

	}

}


int g_epollfd = -1;//epoll句柄
bool g_bStop = false;//应用退出标志
bool g_accept_run_flag = false;//accept触发标志
int g_listenfd = -1;//监听描述符
int g_udpFd = -1;//UDP描述符
int g_timerFd = -1;//定时描述符


pthread_t g_acceptthreadid = 0;
pthread_t g_timerthreadid = 0;
pthread_t g_threadid[WORKER_THREAD_NUM] = { 0 };
pthread_cond_t g_acceptCond;
pthread_mutex_t g_acceptMutex;

pthread_cond_t g_cond /*= PTHREAD_COND_INITIALIZER*/;
pthread_mutex_t g_mutex /*= PTHREAD_MUTEX_INITIALIZER*/;

pthread_mutex_t g_listMutex;
pthread_mutex_t g_mapMutex;

std::list<int> g_activeClients;
std::map<int, ClientInfo_t> g_tcpConnections;
std::map<std::string, ClientInfo_t> g_udpConnections;
//std::map<int, tcprecvpacket_t> g_client_map;//客户端map

const char* kLoopBack = "127.0.0.1";
const uint16_t kTcpPort = 12539;
const uint16_t kUdpPort = 12540;
const uint16_t kTimerCycle = 10;//s =>10s
const uint16_t kMaxTimeout = 2 * kTimerCycle;//s =>20s
static const int kMicroSecondsPerSecond = 1000 * 1000;


typedef void(*sighandler_t)(int);

int pox_system(const char *cmd_line)
{
	//调整的原因：调用system函数时，其中会调用fork()函数，并用子进程去执行命令，父进程等待回收子进程。
	//而初始化的时候，将SIGCHLD信号注册为回调方式处理退出的子线程，此时可能出现进入回调回收了子线程，
	//然后再回到system里的父线程，可能因此不能回收子线程，而出现错误。而命令在子线程里有可能是执行成功的。
	int ret = 0;
	sighandler_t old_handler;
	old_handler = signal(SIGCHLD, SIG_DFL);//返回上一次的行为
	ret = system(cmd_line);
	//（1） - 1 != status
	//（2）WIFEXITED(status)为真
	//（3）0 == WEXITSTATUS(status)
	if (ret == -1)
	{
		//log_warning("system error:%d\n", errno);
	}
	else
	{
		//log_debug("exit ret value:0x%x\n", ret);
		if (WIFEXITED(ret))
		{
			if (0 == WEXITSTATUS(ret))
			{
				//log_debug("run shell script successfully.\n");
			}
			else
			{
				//log_warning("run shell script fail, script exit code: %d\n", WEXITSTATUS(ret));
			}
		}
		else
		{
			//log_warning("exit status = [%d]\n", WEXITSTATUS(ret));
		}
	}

	signal(SIGCHLD, old_handler);//恢复之前的行为

}

int64_t getNowTimestamp()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	int64_t seconds = tv.tv_sec;
	return  (seconds * kMicroSecondsPerSecond + tv.tv_usec);

}

void release_client(int clientfd)
{
	if (epoll_ctl(g_epollfd, EPOLL_CTL_DEL, clientfd, NULL) == -1)
		std::cout << "release client socket failed as call epoll_ctl failed" << std::endl;

	close(clientfd);
}
void set_timer(int seconds, int useconds)
{

	struct timeval temp;

	temp.tv_sec = seconds;

	temp.tv_usec = useconds;

	select(0, NULL, NULL, NULL, &temp);

	return;

}

void prog_exit(int signo)
{
	signal(SIGINT, SIG_IGN);
	signal(SIGKILL, SIG_IGN);
	signal(SIGTERM, SIG_IGN);

	std::cout << "program recv signal " << signo << " to exit." << std::endl;

	//停止定时器
	if (g_timerFd != -1)
	{
		daemonHeart::detail::resetTimerfd(g_timerFd, 0, 0, true);
	}
	g_bStop = true;

	//if (g_timerthreadid != 0)
	//{
	//	pthread_join(g_timerthreadid, NULL);//等待回收线程
	//	g_timerthreadid = 0;
	//}
	
	if (g_acceptthreadid != 0)
	{
		g_accept_run_flag = true;
		pthread_cond_signal(&g_acceptCond);//通知accept线程退出
		pthread_join(g_acceptthreadid, NULL);//等待回收线程
		g_acceptthreadid = 0;
	}

	if (g_threadid[0] != 0)
	{
		struct epoll_event ev;
		ev.data.fd = 0;
		pthread_mutex_lock(&g_listMutex);//通知worker线程退出
		g_activeClients.push_back(ev.data.fd);
		pthread_mutex_unlock(&g_listMutex);
		pthread_cond_broadcast(&g_cond);
		pthread_join(g_threadid[0], NULL);
		g_threadid[0] = 0;
	}

	if (g_epollfd != -1)
	{
		int tempFd = -1;
		pthread_mutex_lock(&g_mapMutex);

		for (std::map<std::string, ClientInfo_t>::iterator it = g_udpConnections.begin(); it != g_udpConnections.end();)
		{
			g_udpConnections.erase(it++);
		}

		for (std::map<int, ClientInfo_t>::iterator it = g_tcpConnections.begin(); it != g_tcpConnections.end();)
		{
			tempFd = it->first;
			release_client(tempFd);//删除tcp客户端描述符
			g_tcpConnections.erase(it++);
		}
		pthread_mutex_unlock(&g_mapMutex);


		epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_listenfd, NULL);//删除监听描述符
		epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_timerFd, NULL);//删除定时器描述符
		epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_udpFd, NULL);//删除udp描述符

		//TODO: 是否需要先调用shutdown()一下？
		shutdown(g_listenfd, SHUT_RDWR);
		close(g_listenfd);
		close(g_timerFd);
		close(g_udpFd);
		close(g_epollfd);
		g_epollfd = -1;
		g_listenfd = -1;
		g_timerFd = -1;
		g_udpFd = -1;

		pthread_cond_destroy(&g_acceptCond);
		pthread_mutex_destroy(&g_acceptMutex);

		pthread_cond_destroy(&g_cond);
		pthread_mutex_destroy(&g_mutex);

		pthread_mutex_destroy(&g_listMutex);
		pthread_mutex_destroy(&g_mapMutex);
	}

	std::cout << "release resource okay. "<< std::endl;
}

void daemon_run()
{
	int pid;
	signal(SIGCHLD, SIG_IGN);
	//1）在父进程中，fork返回新创建子进程的进程ID；
	//2）在子进程中，fork返回0；
	//3）如果出现错误，fork返回一个负值；
	pid = fork();
	if (pid < 0)
	{
		std::cout << "fork error" << std::endl;
		exit(-1);
	}
	//父进程退出，子进程独立运行
	else if (pid > 0) {
		exit(0);
	}
	//之前parent和child运行在同一个session里,parent是会话（session）的领头进程,
	//parent进程作为会话的领头进程，如果exit结束执行的话，那么子进程会成为孤儿进程，并被init收养。
	//执行setsid()之后,child将重新获得一个新的会话(session)id。
	//这时parent退出之后,将不会影响到child了。
	setsid();
	//重定向守护进程的输入输出置为无效。
	int fd;
	fd = open("/dev/null", O_RDWR, 0);
	if (fd != -1)
	{
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
	}
	if (fd > 2)
		close(fd);

}

std::string get_local_ip_by_name(const char *card_name)
{

	int inet_sock;
	struct ifreq ifr;
	char ip[32] = { 0 };

	inet_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (inet_sock < 0)
	{
		std::cout << "local_ip socket err! \n" << std::endl;
		return "";
	}

	strcpy(ifr.ifr_name, card_name);

	ioctl(inet_sock, SIOCGIFADDR, &ifr);

	strcpy(ip, inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
	std::cout << "local_ip: " << ip << std::endl;
	return std::string(ip);

}

int createNonblockingUDP()
{
	int sockfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
	if (sockfd < 0)
	{
		std::cout << "::socket";
	}
	return sockfd;
}


bool create_server_listener(const char* tcpIp, uint16_t tcpPort, const char* udpIp, uint16_t udpPort)
{

	//create tcp scoket
	g_listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);//创建socket
	if (g_listenfd == -1)
		return false;

	int on = 1;
	setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));//重用地址与端口
	//Linux内核(>= 3.9)支持SO_REUSEPORT特性
	//setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEPORT, (char *)&on, sizeof(on));

	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(tcpIp);
	servaddr.sin_port = htons(tcpPort);
	if (bind(g_listenfd, (sockaddr *)&servaddr, sizeof(servaddr)) == -1)//绑定TCP端口与IP
		return false;

	if (listen(g_listenfd, 50) == -1)//设置最大监听数量
		return false;



	//create udp scoket
	g_udpFd = createNonblockingUDP();
	setsockopt(g_udpFd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));//重用地址与端口


	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(udpIp);
	servaddr.sin_port = htons(udpPort);
	if (bind(g_udpFd, (sockaddr *)&servaddr, sizeof(servaddr)) == -1)//绑定UDP端口与IP
		return false;



	//自从Linux2.6.8版本以后，size值其实是没什么用的，不过要大于0，因为内核可以动态的分配大小，所以不需要size这个提示了。
	//它是fd的一个标识说明，用来设置文件close-on-exec状态的。
	//当close-on-exec状态为0时，调用exec时，fd不会被关闭；状态非零时则会被关闭，这样做可以防止fd泄露给执行exec后的进程
	//g_epollfd = epoll_create(1);//创建一个epoll句柄
	g_epollfd = epoll_create1(EPOLL_CLOEXEC);
	if (g_epollfd == -1)
		return false;

	//create timerfd
	g_timerFd = daemonHeart::detail::createTimerfd();
	assert(g_timerFd >= 0);


	struct epoll_event e;
	memset(&e, 0, sizeof(e));
	e.events = EPOLLIN | EPOLLRDHUP;//注册事件：可读/关闭，挂断
	e.data.fd = g_listenfd;
	if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_listenfd, &e) == -1)//注册新的监听fd到g_epollfd。
		return false;

	memset(&e, 0, sizeof(e));
	e.events = EPOLLIN | EPOLLPRI;//注册事件：可读/外带
	e.data.fd = g_udpFd;
	if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_udpFd, &e) == -1)//注册新的UDP-fd到g_epollfd。
		return false;



	/*ClientInfo_t clientInfo;
	int64_t now = getNowTimestamp();
	clientInfo.lastRecvTimestamp = now;
	clientInfo.lastState = CONNECTING;
	clientInfo.flag.pid = 0;

	pthread_mutex_lock(&g_mapMutex);
	g_tcpConnections[g_udpFd] = clientInfo;
	pthread_mutex_unlock(&g_mapMutex);*/




	memset(&e, 0, sizeof(e));
	e.events = EPOLLIN | EPOLLPRI;//注册事件：可读/外带
	e.data.fd = g_timerFd;
	if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_timerFd, &e) == -1)//注册新的timer-fd到g_epollfd。
		return false;





	return true;
}

void* accept_thread_func(void* arg)
{
	std::cout << "accept thread is running." << std::endl;
	struct sockaddr_in clientaddr;
	while (!g_bStop)
	{
		pthread_mutex_lock(&g_acceptMutex);
		while (g_accept_run_flag == false)//防止意外唤醒
		{
			if (g_bStop == true)break;
			pthread_cond_wait(&g_acceptCond, &g_acceptMutex);
		}
		g_accept_run_flag = false;//clear flag
		pthread_mutex_unlock(&g_acceptMutex);

		memset(&clientaddr, 0x00, sizeof(sockaddr_in));
		socklen_t addrlen = sizeof(sockaddr_in);
		int newfd = accept(g_listenfd, (struct sockaddr *)&clientaddr, &addrlen);
		if (newfd == -1)
		{
			std::cout << "Accept fail! " << std::endl;
			continue;
		}
		std::cout << "new client connected: " << inet_ntoa(clientaddr.sin_addr) << ":" << ntohs(clientaddr.sin_port) << std::endl;

		//printf("new client connected: %s:%d \n", inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));

		//将新socket设置为non-blocking
		int oldflag = fcntl(newfd, F_GETFL, 0);
		int newflag = oldflag | O_NONBLOCK;
		if (fcntl(newfd, F_SETFL, newflag) == -1)
		{
			std::cout << "fcntl error, oldflag =" << oldflag << ", newflag = " << newflag << std::endl;
			continue;
		}

		struct epoll_event e;
		memset(&e, 0, sizeof(e));
		e.events = EPOLLIN | EPOLLPRI | EPOLLRDHUP;//事件：可读、外带、挂断(默认水平触发)
		e.data.fd = newfd;
		if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, newfd, &e) == -1)//为新加入的描述符注册
		{
			std::cout << "epoll_ctl error, fd =" << newfd << std::endl;
		}



		//tcprecvpacket_t recvp;
		//recvp.connect_state = CONNECTED;
		//recvp.obj_process_pid = 0;//默认为0
		//gettimeofday(&(recvp.timestamp), NULL);//更新时间戳
		//pthread_mutex_lock(&g_mapMutex);
		//g_client_map[newfd] = recvp;//如果是相同的描述符直接覆盖
		//pthread_mutex_unlock(&g_mapMutex);

		ClientInfo_t clientInfo;
		int64_t now = getNowTimestamp();
		clientInfo.lastRecvTimestamp = now;
		clientInfo.lastState = CONNECTING;
		clientInfo.flag.pid = 0;

		pthread_mutex_lock(&g_mapMutex);
		g_tcpConnections[newfd] = clientInfo;
		//g_client_map[newfd] = recvp;//如果是相同的描述符直接覆盖
		pthread_mutex_unlock(&g_mapMutex);

	}

	std::cout << "exit accept thread." << std::endl;

	return NULL;
}

void* worker_thread_func(void* arg)
{
	std::cout << "worker thread is running." << std::endl;
	//int8_t message[23] = {0};
	//int8_t byteArray[256] = { 0 };
	DeamonExternalMonitorProto_t message;
	memset((void*)&message, 0x00, sizeof(message));
	ClientInfo_t clientInfo;
	int64_t now = 0;
	uint32_t tcpRecvIdx = 0;
	uint32_t udpRecvIdx = 0;
	struct sockaddr_in peerAddr;
	bzero(&peerAddr, sizeof peerAddr);
	socklen_t addrLen = sizeof peerAddr;
	bool recvOkay = false;

	while (!g_bStop)
	{
		int clientfd = -1;
		pthread_mutex_lock(&g_listMutex);
		while (g_activeClients.empty())
		{
			pthread_cond_wait(&g_cond, &g_listMutex);
		}
		clientfd = g_activeClients.front();//取出被激活的描述符
		g_activeClients.pop_front();
		pthread_mutex_unlock(&g_listMutex);

		if ((g_bStop == true) || (clientfd < 0))break;
		//gdb调试时不能实时刷新标准输出，用这个函数刷新标准输出，使信息在屏幕上实时显示出来
		std::cout << std::endl;

		if (clientfd == g_udpFd)//UDP
		{	
			ssize_t nr = recvfrom(clientfd, (message.fragment + udpRecvIdx), ((sizeof message.fragment) - udpRecvIdx), 0, (struct sockaddr *)&peerAddr, &addrLen);
			if (nr < 0)
			{
				if (ETRYAGAIN(errno))//超时，或者被信号中断
				{
					continue;
				}
				else
				{
					std::cout << "recv udp packet err ! " << std::endl;
				}
			}
			else
			{
				std::cout << "recv udp message: " << nr << " bytes. " << "from fd: " << clientfd
					<< ", and IpPort => " << inet_ntoa(peerAddr.sin_addr) << ":" << ntohs(peerAddr.sin_port) << std::endl;

				udpRecvIdx += nr;
				if (udpRecvIdx >= kProtoPacketMaxLen)//需要一个完整包
				{
					std::cout << "recv udp packet okay. " << std::endl;
					udpRecvIdx = 0;
					recvOkay = true;
				}
			}
		}
		else
		{
			int nRecv = recv(clientfd, (message.fragment + tcpRecvIdx), ((sizeof message.fragment) - tcpRecvIdx), 0);
			if (nRecv < 0)
			{
				if (ETRYAGAIN(errno))//超时，或者被信号中断
				{
					continue;
				}			
				else
				{
					std::cout << "recv error, client disconnected, fd = " << clientfd << std::endl;

					pthread_mutex_lock(&g_mapMutex);
					if (g_tcpConnections.find(clientfd) != g_tcpConnections.end())
					{
						release_client(clientfd);//删除客户端描述符
						g_tcpConnections.erase(clientfd);
					}
					pthread_mutex_unlock(&g_mapMutex);
				}
			}
			else if (nRecv == 0)		//对端关闭了socket，这端也关闭。
			{
				std::cout << "peer closed, client disconnected, fd = " << clientfd << std::endl;
				pthread_mutex_lock(&g_mapMutex);
				if (g_tcpConnections.find(clientfd) != g_tcpConnections.end())
				{
					release_client(clientfd);//删除客户端描述符
					g_tcpConnections.erase(clientfd);
				}
				pthread_mutex_unlock(&g_mapMutex);
			}
			else
			{
				std::cout << "recv tcp message: " << nRecv << " bytes. " << "from fd: " << clientfd << std::endl;
				tcpRecvIdx += nRecv;
				if (tcpRecvIdx >= kProtoPacketMaxLen)
				{
					std::cout << "recv tcp packet okay. " << std::endl;
					recvOkay = true;
					tcpRecvIdx = 0;
				}
			}
		}

		if (recvOkay)
		{
			recvOkay = false;
			if (message.packet.header.start_byte == PROTO_HEADER_START)
			{
				char buf[64] = "";
				sprintf(buf, "%d.%d.%d.%d:%u:%lld:%u",
					message.packet.header.flag.ip[0],
					message.packet.header.flag.ip[1],
					message.packet.header.flag.ip[2],
					message.packet.header.flag.ip[3],
					message.packet.header.flag.port,
					message.packet.header.flag.sTime,
					message.packet.header.flag.pid);
				std::string identityKey = buf;
				printf("identityKey: %s \r\n", identityKey.c_str());
				printf("appID: %d \r\n", message.packet.fileds.appID);

				now = getNowTimestamp();

				pthread_mutex_lock(&g_mapMutex);
				if (clientfd == g_udpFd)//UDP客户端
				{		
					//直接下标操作，不存在则直接新插入
					g_udpConnections[identityKey].lastState = WAITHEARTBEAT;
					g_udpConnections[identityKey].lastRecvTimestamp = now;
					memcpy(&g_udpConnections[identityKey].flag, &(message.packet.header.flag), sizeof(HeaderFlag_t));
				}
				else//TCP客户端
				{
					if (g_tcpConnections.find(clientfd) != g_tcpConnections.end())
					{
						g_tcpConnections[clientfd].lastState = WAITHEARTBEAT;
						g_tcpConnections[clientfd].lastRecvTimestamp = now;
						memcpy(&g_tcpConnections[clientfd].flag, &(message.packet.header.flag), sizeof(HeaderFlag_t));
					}
				}
				pthread_mutex_unlock(&g_mapMutex);

			}
			memset((void*)&message, 0x00, sizeof(message));
		}

		std::cout << std::endl;
		//std::string strclientmsg="";
		//char buff[256];
		//bool bError = false;
		//while (true)
		//{
		//	memset(buff, 0, sizeof(buff));
		//	int nRecv = recv(clientfd, buff, 256, 0);
		//	if (nRecv == -1)
		//	{
		//		if (errno == EWOULDBLOCK)//直到资源不可用，说明数据接收完成
		//			break;
		//		else
		//		{
		//			std::cout << "recv error, client disconnected, fd = " << clientfd << std::endl;
		//			release_client(clientfd);
		//			bError = true;
		//			break;
		//		}
		//	}
		//	//对端关闭了socket，这端也关闭。
		//	else if (nRecv == 0)
		//	{
		//		std::cout << "peer closed, client disconnected, fd = " << clientfd << std::endl;
		//		release_client(clientfd);
		//		bError = true;
		//		break;
		//	}
		//	strclientmsg += buff;
		//}
		////出错了，就不要再继续往下执行了
		//if (bError)
		//{
		//	continue;
		//}


		//std::cout << "client msg: " << strclientmsg.size() << ", " << strclientmsg << std::endl;

		
		//recvp.connect_state = WAITHEARTBEAT;
		//gettimeofday(&(recvp.timestamp), NULL);//更新时间戳
		//if (strclientmsg.size() == 10)//长度限定
		//{
		//	pthread_mutex_lock(&g_mapMutex);
		//	std::map<int, tcprecvpacket_t>::iterator it;
		//	it = g_client_map.find(clientfd);
		//	if (it != g_client_map.end())
		//	{
		//		it->second.connect_state = WAITHEARTBEAT;
		//		gettimeofday(&(it->second.timestamp), NULL);//更新时间戳.
		//	}
		//	pthread_mutex_unlock(&g_mapMutex);
		//}

		////将消息加上时间标签后发回
		//time_t now = time(NULL);
		//struct tm* nowstr = localtime(&now);
		//std::ostringstream ostimestr;
		//ostimestr << "[" << nowstr->tm_year + 1900 << "-"
		//	<< std::setw(2) << std::setfill('0') << nowstr->tm_mon + 1 << "-"
		//	<< std::setw(2) << std::setfill('0') << nowstr->tm_mday << " "
		//	<< std::setw(2) << std::setfill('0') << nowstr->tm_hour << ":"
		//	<< std::setw(2) << std::setfill('0') << nowstr->tm_min << ":"
		//	<< std::setw(2) << std::setfill('0') << nowstr->tm_sec << "]server reply: ";
		//strclientmsg.insert(0, ostimestr.str());
		//while (true)
		//{
		//	int nSent = send(clientfd, strclientmsg.c_str(), strclientmsg.length(), 0);
		//	if (nSent == -1)
		//	{
		//		if (errno == EWOULDBLOCK)
		//		{
		//			sleep(10);
		//			continue;
		//		}
		//		else
		//		{
		//			std::cout << "send error, fd = " << clientfd << std::endl;
		//			release_client(clientfd);
		//			break;
		//		}
		//	}
		//	std::cout << "send: " << strclientmsg;
		//	strclientmsg.erase(0, nSent);
		//	if (strclientmsg.empty())
		//		break;
		//}
	}

	std::cout << "exit worker thread." << std::endl;
	return NULL;
}

//void* timer_thread_func(void* arg)
//{
//	
//	std::cout << "timer thread is running." << std::endl;
//	while (!g_bStop)
//	{
//		//std::cout << std::endl;
//		int ret = 0;
//		ret = pthread_mutex_trylock(&g_mapMutex);
//		if (ret != 0 && ret == EBUSY)
//		{
//			set_timer(0, 10);//10ms
//			continue;
//		}
//		else
//		{
//			std::map<int, tcprecvpacket_t>::iterator it = g_client_map.begin();
//			while (it != g_client_map.end())
//			{
//				if (g_bStop)break;
//				if (it->second.connect_state == WAITHEARTBEAT)
//				{
//					timeval current_tv;
//					gettimeofday(&(current_tv), NULL);//获取当前时间戳；
//					int64_t time_diff_ms = 0;//同一换算成ms
//					int64_t timeout_threshold_ms = 49000;//标准门限
//
//					time_diff_ms = ((int64_t)current_tv.tv_sec * 1000 + current_tv.tv_usec / 1000)
//						- ((int64_t)it->second.timestamp.tv_sec * 1000 + it->second.timestamp.tv_usec / 1000);
//
//					if (time_diff_ms > timeout_threshold_ms)//触发超时
//					{
//						std::cout << "fd:" << it->first <<", "<<"timeout! Reboot device after 3s"<< std::endl;
//						g_bStop = true;
//						//char tmp[100];
//						//bzero(tmp, 100);
//						//sprintf(tmp, "reboot");//重启设备
//						//pox_system(tmp);
//						set_timer(3, 0);//10ms
//					}
//					else//检查下一个
//					{
//						std::cout << "fd:" << it->first <<", "<< "heartbeat checking." << std::endl;
//					}
//				}
//				else
//				{
//				
//				}
//				it++;
//			}
//
//			pthread_mutex_unlock(&g_mapMutex);
//
//		}
//
//		set_timer(7, 20);
//	}
//
//	std::cout << "exit timer thread ." << std::endl;
//
//}
int main(int argc, char* argv[])
{

	//short port = 0;
	int ch;
	bool bdaemon = false;
	//const char * netcard_name = "eth0:virtual-7";
	std::string server_ip = "";
	while ((ch = getopt(argc, argv, "p:d:s:")) != -1)
	{
		switch (ch)
		{
		case 'd':
			bdaemon = true;
			break;
		case 'p':
			//port = atol(optarg);//获取外部设置的端口号
		case 's':
			printf("Have option -s \r\n");
			printf("The argument of -s is %s\n\n", optarg);
			printf("start debug mode. \r\n");
			server_ip = optarg;//获取外部设置的ip
			break;
		}
	}

	if (bdaemon)
		daemon_run();

	//if (port == 0)
	//	port = 12345;

	//server_ip = get_local_ip_by_name(netcard_name);
	//if (server_ip.empty() == true)
	//{
	//	std::cout << "Unable to get server ip" << std::endl;
	//	return -1;
	//}

	g_activeClients.clear();
	//设置信号处理
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, prog_exit);
	signal(SIGKILL, prog_exit);
	signal(SIGTERM, prog_exit);

	pthread_cond_init(&g_acceptCond, NULL);
	pthread_mutex_init(&g_acceptMutex, NULL);

	pthread_cond_init(&g_cond, NULL);
	pthread_mutex_init(&g_mutex, NULL);

	pthread_mutex_init(&g_listMutex, NULL);
	pthread_mutex_init(&g_mapMutex, NULL);



	//if (!create_server_listener(server_ip.c_str(), kTcpPort, server_ip.c_str(), kUdpPort))
	if (!create_server_listener(kLoopBack, kTcpPort, kLoopBack, kUdpPort))
	{
		std::cout << "Unable to create listen server: ip=127.0.0.1, port=" << kTcpPort << "." << std::endl;
		return -1;
	}


	pthread_create(&g_acceptthreadid, NULL, accept_thread_func, NULL);

	//启动工作线程
	for (int i = 0; i < WORKER_THREAD_NUM; ++i)
	{
		pthread_create(&g_threadid[i], NULL, worker_thread_func, NULL);
	}

	//pthread_create(&g_timerthreadid, NULL, timer_thread_func, NULL);

	//启动定时器
	daemonHeart::detail::resetTimerfd(g_timerFd, 0, kTimerCycle, false);

	int64_t now = 0;
	char shellCmd[100] = { 0 };
	while (!g_bStop)
	{
		struct epoll_event ev[10];
		int n = epoll_wait(g_epollfd, ev, 10, 10);//10ms
		if (n == 0)//超时
			continue;
		else if (n < 0)
		{
			std::cout << "epoll_wait error" << std::endl;
			continue;
		}
		else
		{
			std::cout << "epoll_wait take =>  " << n  << " fd(s)." <<  std::endl;
			//int m = min(n, 1024);//最大只能取到1024
			for (int i = 0; i < n; ++i)
			{
				//通知接收连接线程接收新连接
				if (ev[i].data.fd == g_listenfd)
				{
					std::cout << "epoll_wait: g_listenfd. " << std::endl;
					g_accept_run_flag = true;
					pthread_cond_signal(&g_acceptCond);
				}
				else if (ev[i].data.fd == g_timerFd)//定时任务事件
				{
					std::cout << "epoll_wait: g_timerFd. " << std::endl;
					//读取事件，如果不读取由于Epoll水平触发会导致一直有IO事件产生
					//非阻塞的描述，不用担心线程堵塞
					daemonHeart::detail::readTimerfd(g_timerFd);
					now = getNowTimestamp();

					pthread_mutex_lock(&g_mapMutex);
					{
						//polling udp client
						for (std::map<std::string, ClientInfo_t>::iterator it = g_udpConnections.begin(); it != g_udpConnections.end();)
						{
							if (it->second.lastState == WAITHEARTBEAT)
							{
								if ((it->second.lastRecvTimestamp) < (now - kMaxTimeout*kMicroSecondsPerSecond))
								{
									printf("udp-identity=>%s heart timeout ! \r\n", it->first.c_str());
									printf("client info: \r\n");
									printf("ip: %d.%d.%d.%d \r\n", it->second.flag.ip[0], it->second.flag.ip[1],
										it->second.flag.ip[2], it->second.flag.ip[3]);
									printf("port: %d \r\n", it->second.flag.port);
									printf("start_time: %lld \r\n", it->second.flag.sTime);
									printf("pid: %d \r\n", it->second.flag.pid);
									it->second.lastState = TIMEOUT;
									memset(shellCmd, 0x00, sizeof(shellCmd));
									sprintf(shellCmd, "kill -s 9 %d", it->second.flag.pid);
									pox_system(shellCmd);
									//杀死对方进程后，udp-client需要手动注销
									g_udpConnections.erase(it++);//先删除当前，然后令当前迭代器指向其后继。

								}
								else
								{
									printf("\r\nudp-identity=>%s heart oaky. \r\n", it->first.c_str());
									++it;
								}
							}
						}

						//polling tcp client
						for (std::map<int, ClientInfo_t>::iterator it = g_tcpConnections.begin(); it != g_tcpConnections.end();)
						{
							//if (it->second.lastState == WAITHEARTBEAT)
							//{
								if ((it->second.lastRecvTimestamp) < (now - kMaxTimeout*kMicroSecondsPerSecond))
								{
									if (it->second.lastState == WAITHEARTBEAT)
									{
										printf("fd=>%d heart timeout ! \r\n", it->first);
										printf("client info: \r\n");
										printf("ip: %d.%d.%d.%d \r\n", it->second.flag.ip[0], it->second.flag.ip[1],
											it->second.flag.ip[2], it->second.flag.ip[3]);
										printf("port: %d \r\n", it->second.flag.port);
										printf("start_time: %lld \r\n", it->second.flag.sTime);
										printf("pid: %d \r\n", it->second.flag.pid);
										it->second.lastState = TIMEOUT;
										sprintf(shellCmd, "kill -s 9 %d", it->second.flag.pid);
										pox_system(shellCmd);
										//杀死对方进程后，
										//操作系统会关闭意外退出进程中使用的tcp-socket，并会往本进程发送FIN分节，此时服务端即可注销客户端描述符									
										++it;
									}
									else//TIMEOUT or CONNECTING
									{
										printf("fd=>%d malicious connection! \r\n", it->first);
										release_client(it->first);//删除客户端描述符
										g_tcpConnections.erase(it++);//先把当前值传到erase里，自增，然后执行erase.
									}
								}
								else
								{
									printf("\r\ntcp-fd=>%d heart oaky. \r\n", it->first);
									++it;
								}
							//}

						}
					}
					pthread_mutex_unlock(&g_mapMutex);
					
				}
				//else if (ev[i].data.fd == g_udpFd)//UDP可读事件
				//{
				//}
				//通知普通工作线程接收数据
				else//客户端可读事件(tcp/udp)
				{
					pthread_mutex_lock(&g_listMutex);
					g_activeClients.push_back(ev[i].data.fd);
					pthread_mutex_unlock(&g_listMutex);
					pthread_cond_signal(&g_cond);
					//std::cout << "signal" << std::endl;
				}

			}
		}

	}
	
	prog_exit(0);

	return 0;

}