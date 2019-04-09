/**
*@desc:   crash��ط���������
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

#define WORKER_THREAD_NUM   2
#define min(a, b) ((a <= b) ? (a) : (b)) 
const uint16_t kProtoPacketMaxLen = 23;//bytes

#pragma pack(1)
enum	CLIENTSTATE
{
	CONNECTING,//��������
	WAITHEARTBEAT,//�ȴ������������
	DISCONNECTED//�ѶϿ�
};

typedef struct{

	timeval		timestamp;//ָ��ʱ���
	int			obj_process_pid;//Ŀ����̵�pid
	bool        connect_state;//Ŀ������״̬

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

	CLIENTSTATE		connectState;//Ŀ������״̬
	int64_t			lastTimestamp;
	HeaderFlag_t	flag;

}ClientInfo_t;


#pragma pack()



namespace daemon
{
	namespace detail
	{

		int createEventfd()
		{
			int evtfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
			if (evtfd < 0)
			{
				log_warning("Failed in eventfd");
				abort();
			}
			return evtfd;
		}

		//��ȡ�¼����������ȡ����Epollˮƽ�����ᵼ��һֱ��IO�¼�����
		void readEventfd(int eventfd)
		{
			uint64_t one = 1;
			ssize_t n = read(eventfd, &one, sizeof one);
		}

		int createTimerfd()
		{
			/*
			��CLOCK_REALTIME�෴�������Ծ���ʱ��Ϊ׼����ȡ��ʱ��Ϊϵͳ���һ�����������ڵ�ʱ�䣬
			����ϵͳʱ�����ûӰ�졣
			*/
			int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
			if (timerfd < 0)
			{
				log_warning("Failed in timerfd_create");
			}
			return timerfd;
		}

		//��ȡ�¼����������ȡ����Epollˮƽ�����ᵼ��һֱ��IO�¼�����
		void readTimerfd(int timerfd)
		{
			uint64_t howmany;
			ssize_t n = read(timerfd, &howmany, sizeof(howmany));
		}

		//��֧�־���Ϊ������ڶ�ʱ
		//timeSeconds == 0��ֹͣ��ʱ��
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
				newValue.it_value.tv_sec = delaySeconds;//���õ�һ�Σ�����ʱdelaySeconds����
				newValue.it_value.tv_nsec = 1000000;
				newValue.it_interval.tv_sec = intervalSeconds;//���ڼ��
				newValue.it_interval.tv_nsec = 0;
			}

			//it_interval��Ϊ0���ʾ�������Զ�ʱ����
			//it_value��it_interval��Ϊ0��ʾֹͣ��ʱ����
			//����it_valueΪ0�ǲ��ᴥ���źŵģ�����Ҫ�ܴ����źţ�it_value�ô���0�����it_intervalΪ�㣬ֻ����ʱ�����ᶨʱ��Ҳ����˵ֻ�ᴥ��һ���ź�)��

			/*
			�ú����Ĺ���Ϊ������ֹͣ��ʱ����
			��һ������fdΪ�����timerfd_create()�������صĶ�ʱ���ļ���������
			�ڶ�������flagsΪ0��ʾ��Զ�ʱ����ΪTFD_TIMER_ABSTIME��ʾ���Զ�ʱ����
			����������new_value�������ó�ʱʱ�䣬Ϊ0��ʾֹͣ��ʱ����
			���ĸ�����Ϊԭ���ĳ�ʱʱ�䣬һ����ΪNULL��

			��Ҫע��������ǿ���ͨ��clock_gettime��ȡ��ǰʱ�䣬
			����Ǿ��Զ�ʱ������ô���ǵû�ȡ1970.1.1����ǰʱ��(CLOCK_REALTIME),�ټ��������Լ����Ķ�ʱʱ�䡣
			������Զ�ʱ����Ҫ��ȡ����ϵͳ���ο�����Ŀǰ��ʱ�������Ҫ����ʱ��(����ȡCLOCK_MONOTONICʱ��)

			struct itimerspec {
			struct timespec it_interval;  // Interval for periodic timer
			struct timespec it_value;     // Initial expiration
			};

			*/
			int ret = timerfd_settime(timerfd, 0, &newValue, &oldValue);
			if (ret)
			{
				log_warning("timerfd_settime()");
			}
		}

	}

}


int g_epollfd = -1;//epoll���
bool g_bStop = false;//Ӧ���˳���־
bool g_accept_run_flag = false;//accept������־
int g_listenfd = -1;//����������
int g_udpFd = -1;//UDP������
int g_timerFd = -1;//��ʱ������


pthread_t g_acceptthreadid = 0;
pthread_t g_timerthreadid = 0;
pthread_t g_threadid[WORKER_THREAD_NUM] = { 0 };
pthread_cond_t g_acceptcond;
pthread_mutex_t g_acceptmutex;

pthread_cond_t g_cond /*= PTHREAD_COND_INITIALIZER*/;
pthread_mutex_t g_mutex /*= PTHREAD_MUTEX_INITIALIZER*/;

pthread_mutex_t g_clientmutex;
pthread_mutex_t g_mapmutex;

std::list<int> g_activeClients;
std::map<int, ClientInfo_t> g_tcpConnections;
//std::map<int, tcprecvpacket_t> g_client_map;//�ͻ���map

const char* kLoopBack = "127.0.0.1";
const uint16_t kTcpPort = 12539;
const uint16_t kUdpPort = 12540;
const uint16_t kTimerCycle = 10;//s =>10s
const uint16_t kMaxTimeout = 2 * kTimerCycle;//s =>20s
static const int kMicroSecondsPerSecond = 1000 * 1000;


typedef void(*sighandler_t)(int);

int pox_system(const char *cmd_line)
{
	//������ԭ�򣺵���system����ʱ�����л����fork()�����������ӽ���ȥִ����������̵ȴ������ӽ��̡�
	//����ʼ����ʱ�򣬽�SIGCHLD�ź�ע��Ϊ�ص���ʽ�����˳������̣߳���ʱ���ܳ��ֽ���ص����������̣߳�
	//Ȼ���ٻص�system��ĸ��̣߳�������˲��ܻ������̣߳������ִ��󡣶����������߳����п�����ִ�гɹ��ġ�
	int ret = 0;
	sighandler_t old_handler;
	old_handler = signal(SIGCHLD, SIG_DFL);//������һ�ε���Ϊ
	ret = system(cmd_line);
	//��1�� - 1 != status
	//��2��WIFEXITED(status)Ϊ��
	//��3��0 == WEXITSTATUS(status)
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

	signal(SIGCHLD, old_handler);//�ָ�֮ǰ����Ϊ

}

int64_t getNowTimestamp()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	int64_t seconds = tv.tv_sec;
	return  (seconds * kMicroSecondsPerSecond + tv.tv_usec);

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

	//ֹͣ��ʱ��
	daemon::detail::resetTimerfd(g_timerFd, 0, 0, true);
	g_bStop = true;

	if (g_timerthreadid != 0)
	{
		pthread_join(g_timerthreadid, NULL);//�ȴ������߳�
		g_timerthreadid = 0;
	}
	
	if (g_acceptthreadid != 0)
	{
		g_accept_run_flag = true;
		pthread_cond_signal(&g_acceptcond);//֪ͨaccept�߳��˳�
		pthread_join(g_acceptthreadid, NULL);//�ȴ������߳�
		g_acceptthreadid = 0;
	}

	if (g_threadid[0] != 0 && g_threadid[1] != 0)
	{
		struct epoll_event ev;
		ev.data.fd = 0;
		pthread_mutex_lock(&g_clientmutex);//֪ͨworker�߳��˳�
		g_activeClients.push_back(ev.data.fd);
		g_activeClients.push_back(ev.data.fd);
		pthread_mutex_unlock(&g_clientmutex);
		pthread_cond_broadcast(&g_cond);
		pthread_join(g_threadid[0], NULL);
		pthread_join(g_threadid[1], NULL);//�ȴ������߳�
		g_threadid[0] = 0;
		g_threadid[1] = 0;
	}

	if (g_epollfd != -1)
	{
		while ()
		{

		}
		epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_listenfd, NULL);//ɾ������������
		epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_udpFd, NULL);//ɾ������������
		epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_timerFd, NULL);//ɾ������������


		//TODO: �Ƿ���Ҫ�ȵ���shutdown()һ�£�
		shutdown(g_listenfd, SHUT_RDWR);
		close(g_listenfd);
		close(g_epollfd);
		close(g_timerFd);
		g_epollfd = 0;
		g_listenfd = 0;
		g_timerFd = 0;

		pthread_cond_destroy(&g_acceptcond);
		pthread_mutex_destroy(&g_acceptmutex);

		pthread_cond_destroy(&g_cond);
		pthread_mutex_destroy(&g_mutex);

		pthread_mutex_destroy(&g_clientmutex);
		pthread_mutex_destroy(&g_mapmutex);
	}

	std::cout << "release resource okay. "<< std::endl;
}

void daemon_run()
{
	int pid;
	signal(SIGCHLD, SIG_IGN);
	//1���ڸ������У�fork�����´����ӽ��̵Ľ���ID��
	//2�����ӽ����У�fork����0��
	//3��������ִ���fork����һ����ֵ��
	pid = fork();
	if (pid < 0)
	{
		std::cout << "fork error" << std::endl;
		exit(-1);
	}
	//�������˳����ӽ��̶�������
	else if (pid > 0) {
		exit(0);
	}
	//֮ǰparent��child������ͬһ��session��,parent�ǻỰ��session������ͷ����,
	//parent������Ϊ�Ự����ͷ���̣����exit����ִ�еĻ�����ô�ӽ��̻��Ϊ�¶����̣�����init������
	//ִ��setsid()֮��,child�����»��һ���µĻỰ(session)id��
	//��ʱparent�˳�֮��,������Ӱ�쵽child�ˡ�
	setsid();
	//�ض����ػ����̵����������Ϊ��Ч��
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
	g_listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);//����socket
	if (g_listenfd == -1)
		return false;

	int on = 1;
	setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));//���õ�ַ��˿�
	//Linux�ں�(>= 3.9)֧��SO_REUSEPORT����
	//setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEPORT, (char *)&on, sizeof(on));

	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(tcpIp);
	servaddr.sin_port = htons(tcpPort);
	if (bind(g_listenfd, (sockaddr *)&servaddr, sizeof(servaddr)) == -1)//��TCP�˿���IP
		return false;

	if (listen(g_listenfd, 50) == -1)//��������������
		return false;



	//create udp scoket
	g_udpFd = createNonblockingUDP();
	setsockopt(g_udpFd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));//���õ�ַ��˿�


	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(udpIp);
	servaddr.sin_port = htons(udpPort);
	if (bind(g_udpFd, (sockaddr *)&servaddr, sizeof(servaddr)) == -1)//��UDP�˿���IP
		return false;



	//�Դ�Linux2.6.8�汾�Ժ�sizeֵ��ʵ��ûʲô�õģ�����Ҫ����0����Ϊ�ں˿��Զ�̬�ķ����С�����Բ���Ҫsize�����ʾ�ˡ�
	//����fd��һ����ʶ˵�������������ļ�close-on-exec״̬�ġ�
	//��close-on-exec״̬Ϊ0ʱ������execʱ��fd���ᱻ�رգ�״̬����ʱ��ᱻ�رգ����������Է�ֹfdй¶��ִ��exec��Ľ���
	//g_epollfd = epoll_create(1);//����һ��epoll���
	g_epollfd = epoll_create1(EPOLL_CLOEXEC);
	if (g_epollfd == -1)
		return false;

	//create timerfd
	g_timerFd = daemon::detail::createTimerfd();
	assert(g_timerFd >= 0);


	struct epoll_event e;
	memset(&e, 0, sizeof(e));
	e.events = EPOLLIN | EPOLLRDHUP;//ע���¼����ɶ�/�رգ��Ҷ�
	e.data.fd = g_listenfd;
	if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_listenfd, &e) == -1)//ע���µļ���fd��g_epollfd��
		return false;

	memset(&e, 0, sizeof(e));
	e.events = EPOLLIN | POLLPRI;//ע���¼����ɶ�/���
	e.data.fd = g_udpFd;
	if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_udpFd, &e) == -1)//ע���µ�UDP-fd��g_epollfd��
		return false;

	memset(&e, 0, sizeof(e));
	e.events = EPOLLIN | POLLPRI;//ע���¼����ɶ�/���
	e.data.fd = g_timerFd;
	if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_timerFd, &e) == -1)//ע���µ�timer-fd��g_epollfd��
		return false;


	return true;
}
void release_client(int clientfd)
{
	if (epoll_ctl(g_epollfd, EPOLL_CTL_DEL, clientfd, NULL) == -1)
		std::cout << "release client socket failed as call epoll_ctl failed" << std::endl;

	close(clientfd);
}

void* accept_thread_func(void* arg)
{
	std::cout << "accept thread is running." << std::endl;
	struct sockaddr_in clientaddr;
	while (!g_bStop)
	{
		pthread_mutex_lock(&g_acceptmutex);
		while (g_accept_run_flag == false)//��ֹ���⻽��
		{
			if (g_bStop == true)break;
			pthread_cond_wait(&g_acceptcond, &g_acceptmutex);
		}
		g_accept_run_flag = false;//clear flag
		pthread_mutex_unlock(&g_acceptmutex);

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

		//����socket����Ϊnon-blocking
		int oldflag = fcntl(newfd, F_GETFL, 0);
		int newflag = oldflag | O_NONBLOCK;
		if (fcntl(newfd, F_SETFL, newflag) == -1)
		{
			std::cout << "fcntl error, oldflag =" << oldflag << ", newflag = " << newflag << std::endl;
			continue;
		}

		struct epoll_event e;
		memset(&e, 0, sizeof(e));
		e.events = EPOLLIN | POLLPRI| EPOLLRDHUP;//�¼����ɶ���������Ҷ�(Ĭ��ˮƽ����)
		e.data.fd = newfd;
		if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, newfd, &e) == -1)//Ϊ�¼����������ע��
		{
			std::cout << "epoll_ctl error, fd =" << newfd << std::endl;
		}



		//tcprecvpacket_t recvp;
		//recvp.connect_state = CONNECTED;
		//recvp.obj_process_pid = 0;//Ĭ��Ϊ0
		//gettimeofday(&(recvp.timestamp), NULL);//����ʱ���
		//pthread_mutex_lock(&g_mapmutex);
		//g_client_map[newfd] = recvp;//�������ͬ��������ֱ�Ӹ���
		//pthread_mutex_unlock(&g_mapmutex);

		ClientInfo_t clientInfo;
		int64_t now = getNowTimestamp();
		clientInfo.lastTimestamp = now;
		clientInfo.connectState = CONNECTING;

		pthread_mutex_lock(&g_mapmutex);
		g_tcpConnections[newfd] = clientInfo;
		//g_client_map[newfd] = recvp;//�������ͬ��������ֱ�Ӹ���
		pthread_mutex_unlock(&g_mapmutex);

	}

	std::cout << "exit accept thread." << std::endl;

	return NULL;
}

void* worker_thread_func(void* arg)
{
	std::cout << "worker thread is running." << std::endl;
	//int8_t message[23] = {0};
	DeamonExternalMonitorProto_t message;
	struct sockaddr peerAddr;
	bzero(&peerAddr, sizeof peerAddr);
	socklen_t addrLen = sizeof peerAddr;

	while (!g_bStop)
	{
		int clientfd = -1;
		pthread_mutex_lock(&g_clientmutex);
		while (g_activeClients.empty())
		{
			pthread_cond_wait(&g_cond, &g_clientmutex);
		}
		clientfd = g_activeClients.front();//ȡ���������������
		g_activeClients.pop_front();
		pthread_mutex_unlock(&g_clientmutex);

		if ((g_bStop == true) || (clientfd < 0))break;
		//gdb����ʱ����ʵʱˢ�±�׼��������������ˢ�±�׼�����ʹ��Ϣ����Ļ��ʵʱ��ʾ����
		std::cout << std::endl;

		if (clientfd == g_udpFd)
		{
			memset(message, 0x00, sizeof(message));
			ssize_t nr = recvfrom(clientfd, message.fragment, sizeof message.fragment, 0, &peerAddr, &addrLen);
			std::cout << "recv udp message: " << nr << " bytes. "<< std::endl;
			if ((nr > 0) && (nr <= kProtoPacketMaxLen))
			{
				printf("ip: %d.%d.%d.%d \r\n", message.packet.header.flag.ip[0], message.packet.header.flag.ip[1],
					message.packet.header.flag.ip[2], message.packet.header.flag.ip[3]);
				printf("port: %d \r\n", message.packet.header.flag.port);
				printf("start_time: %ld \r\n", message.packet.header.flag.sTime);
				printf("pid: %d \r\n", message.packet.header.flag.pid);
				printf("appID: %d \r\n", message.packet.fileds.appID);
				//std::cout << "ip: " << std::endl;
			}
		}
		else
		{
			std::cout << "recv tcp message. " << std::endl;

		}


		//std::string strclientmsg="";
		//char buff[256];
		//bool bError = false;
		//while (true)
		//{
		//	memset(buff, 0, sizeof(buff));
		//	int nRecv = recv(clientfd, buff, 256, 0);
		//	if (nRecv == -1)
		//	{
		//		if (errno == EWOULDBLOCK)//ֱ����Դ�����ã�˵�����ݽ������
		//			break;
		//		else
		//		{
		//			std::cout << "recv error, client disconnected, fd = " << clientfd << std::endl;
		//			release_client(clientfd);
		//			bError = true;
		//			break;
		//		}
		//	}
		//	//�Զ˹ر���socket�����Ҳ�رա�
		//	else if (nRecv == 0)
		//	{
		//		std::cout << "peer closed, client disconnected, fd = " << clientfd << std::endl;
		//		release_client(clientfd);
		//		bError = true;
		//		break;
		//	}
		//	strclientmsg += buff;
		//}
		////�����ˣ��Ͳ�Ҫ�ټ�������ִ����
		//if (bError)
		//{
		//	continue;
		//}


		//std::cout << "client msg: " << strclientmsg.size() << ", " << strclientmsg << std::endl;

		
		//recvp.connect_state = WAITHEARTBEAT;
		//gettimeofday(&(recvp.timestamp), NULL);//����ʱ���
		//if (strclientmsg.size() == 10)//�����޶�
		//{
		//	pthread_mutex_lock(&g_mapmutex);
		//	std::map<int, tcprecvpacket_t>::iterator it;
		//	it = g_client_map.find(clientfd);
		//	if (it != g_client_map.end())
		//	{
		//		it->second.connect_state = WAITHEARTBEAT;
		//		gettimeofday(&(it->second.timestamp), NULL);//����ʱ���.
		//	}
		//	pthread_mutex_unlock(&g_mapmutex);
		//}

		////����Ϣ����ʱ���ǩ�󷢻�
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
//		ret = pthread_mutex_trylock(&g_mapmutex);
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
//					gettimeofday(&(current_tv), NULL);//��ȡ��ǰʱ�����
//					int64_t time_diff_ms = 0;//ͬһ�����ms
//					int64_t timeout_threshold_ms = 49000;//��׼����
//
//					time_diff_ms = ((int64_t)current_tv.tv_sec * 1000 + current_tv.tv_usec / 1000)
//						- ((int64_t)it->second.timestamp.tv_sec * 1000 + it->second.timestamp.tv_usec / 1000);
//
//					if (time_diff_ms > timeout_threshold_ms)//������ʱ
//					{
//						std::cout << "fd:" << it->first <<", "<<"timeout! Reboot device after 3s"<< std::endl;
//						g_bStop = true;
//						//char tmp[100];
//						//bzero(tmp, 100);
//						//sprintf(tmp, "reboot");//�����豸
//						//pox_system(tmp);
//						set_timer(3, 0);//10ms
//					}
//					else//�����һ��
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
//			pthread_mutex_unlock(&g_mapmutex);
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

	short port = 0;
	int ch;
	bool bdaemon = false;
	const char * netcard_name = "eth0:virtual-7";
	std::string server_ip = "";
	while ((ch = getopt(argc, argv, "p:d")) != -1)
	{
		switch (ch)
		{
		case 'd':
			bdaemon = true;
			break;
		case 'p':
			port = atol(optarg);//��ȡ�ⲿ���õĶ˿ں�
			break;
		}
	}

	if (bdaemon)
		daemon_run();

	if (port == 0)
		port = 12345;

	//server_ip = get_local_ip_by_name(netcard_name);
	//if (server_ip.empty() == true)
	//{
	//	std::cout << "Unable to get server ip" << std::endl;
	//	return -1;
	//}
	if (!create_server_listener(kLoopBack, kTcpPort, kLoopBack, kUdpPort))
	{
		std::cout << "Unable to create listen server: ip=127.0.0.1, port=" << kTcpPort << "." << std::endl;
		return -1;
	}

	g_activeClients.clear();
	//�����źŴ���
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, prog_exit);
	signal(SIGKILL, prog_exit);
	signal(SIGTERM, prog_exit);

	pthread_cond_init(&g_acceptcond, NULL);
	pthread_mutex_init(&g_acceptmutex, NULL);

	pthread_cond_init(&g_cond, NULL);
	pthread_mutex_init(&g_mutex, NULL);

	pthread_mutex_init(&g_clientmutex, NULL);
	pthread_mutex_init(&g_mapmutex, NULL);

	pthread_create(&g_acceptthreadid, NULL, accept_thread_func, NULL);

	//���������߳�
	for (int i = 0; i < WORKER_THREAD_NUM; ++i)
	{
		pthread_create(&g_threadid[i], NULL, worker_thread_func, NULL);
	}

	//pthread_create(&g_timerthreadid, NULL, timer_thread_func, NULL);

	//������ʱ��
	daemon::detail::resetTimerfd(g_timerFd, 0, kTimerCycle, false);


	while (!g_bStop)
	{
		struct epoll_event ev[10];
		int n = epoll_wait(g_epollfd, ev, 10, 10);//10ms
		if (n == 0)//��ʱ
			continue;
		else if (n < 0)
		{
			std::cout << "epoll_wait error" << std::endl;
			continue;
		}
		else
		{
			//int m = min(n, 1024);//���ֻ��ȡ��1024
			for (int i = 0; i < n; ++i)
			{
				//֪ͨ���������߳̽���������
				if (ev[i].data.fd == g_listenfd)
				{
					g_accept_run_flag = true;
					pthread_cond_signal(&g_acceptcond);
				}
				else if (ev[i].data.fd == g_timerFd)//��ʱ�����¼�
				{
					std::cout << "epoll_wait: g_timerFd. " << std::endl;
				}
				//else if (ev[i].data.fd == g_udpFd)//UDP�ɶ��¼�
				//{
				//}
				//֪ͨ��ͨ�����߳̽�������
				else//�ͻ��˿ɶ��¼�(tcp/udp)
				{
					pthread_mutex_lock(&g_clientmutex);
					g_activeClients.push_back(ev[i].data.fd);
					pthread_mutex_unlock(&g_clientmutex);
					pthread_cond_signal(&g_cond);
					//std::cout << "signal" << std::endl;
				}

			}
		}

	}
	
	prog_exit(0);

	return 0;

}