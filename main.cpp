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
#include <sys/epoll.h>
#include <signal.h>     //for signal()
#include <pthread.h>
#include <semaphore.h>
#include <list>
#include <errno.h>
#include <time.h>
#include <sstream>
#include <iomanip> //for std::setw()/setfill()
#include <stdlib.h>

#define WORKER_THREAD_NUM   2
#define min(a, b) ((a <= b) ? (a) : (b)) 

int g_epollfd = 0;//epoll���
bool g_bStop = false;//Ӧ���˳���־
bool g_accept_run_flag = false;//accept������־
int g_listenfd = 0;//����������


pthread_t g_acceptthreadid = 0;
pthread_t g_threadid[WORKER_THREAD_NUM] = { 0 };
pthread_cond_t g_acceptcond;
pthread_mutex_t g_acceptmutex;

pthread_cond_t g_cond /*= PTHREAD_COND_INITIALIZER*/;
pthread_mutex_t g_mutex /*= PTHREAD_MUTEX_INITIALIZER*/;

pthread_mutex_t g_clientmutex;

void prog_exit(int signo)
{
	::signal(SIGINT, SIG_IGN);
	::signal(SIGKILL, SIG_IGN);
	::signal(SIGTERM, SIG_IGN);

	std::cout << "program recv signal " << signo << " to exit." << std::endl;

	g_bStop = true;

	g_accept_run_flag = true;
	pthread_cond_signal(&g_acceptcond);//֪ͨaccept�߳��˳�
	pthread_join(g_acceptthreadid, NULL);//�ȴ������߳�

	struct epoll_event ev;
	ev.data.fd = 0;
	pthread_mutex_lock(&g_clientmutex);//֪ͨworker�߳��˳�
	g_listClients.push_back(ev.data.fd);
	g_listClients.push_back(ev.data.fd);
	pthread_mutex_unlock(&g_clientmutex);
	pthread_cond_broadcast(&g_cond);
	pthread_join(g_threadid[0], NULL);
	pthread_join(g_threadid[1], NULL);//�ȴ������߳�

	epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_listenfd, NULL);//ɾ������������

	//TODO: �Ƿ���Ҫ�ȵ���shutdown()һ�£�
	shutdown(g_listenfd, SHUT_RDWR);
	close(g_listenfd);
	close(g_epollfd);

	pthread_cond_destroy(&g_acceptcond);
	pthread_mutex_destroy(&g_acceptmutex);

	pthread_cond_destroy(&g_cond);
	pthread_mutex_destroy(&g_mutex);

	pthread_mutex_destroy(&g_clientmutex);
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

	return std::string(ip);

}

bool create_server_listener(const char* ip, short port)
{
	g_listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);//����socket
	if (g_listenfd == -1)
		return false;

	int on = 1;
	setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));//���õ�ַ��˿�
	setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEPORT, (char *)&on, sizeof(on));

	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(ip);
	servaddr.sin_port = htons(port);
	if (bind(g_listenfd, (sockaddr *)&servaddr, sizeof(servaddr)) == -1)//�󶨶˿���IP
		return false;

	if (listen(g_listenfd, 50) == -1)//��������������
		return false;

	g_epollfd = epoll_create(1);//����һ��epoll���
	if (g_epollfd == -1)
		return false;

	struct epoll_event e;
	memset(&e, 0, sizeof(e));
	e.events = EPOLLIN | EPOLLRDHUP;//ע���¼����ɶ�/�رգ��Ҷ�
	e.data.fd = g_listenfd;
	if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_listenfd, &e) == -1)//ע���µļ���fd��g_epollfd��
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
	while (!g_bStop)
	{
		pthread_mutex_lock(&g_acceptmutex);
		while (g_accept_run_flag == false)//��ֹ���⻽��
		{
			if (g_bStop == true)break;
			pthread_cond_wait(&g_acceptcond, &g_acceptmutex);
		}

		struct sockaddr_in clientaddr;
		socklen_t addrlen;
		int newfd = accept(g_listenfd, (struct sockaddr *)&clientaddr, &addrlen);
		pthread_mutex_unlock(&g_acceptmutex);
		if (newfd == -1)
		{
			std::cout << "Accept fail! " << std::endl;
			continue;
		}
		std::cout << "new client connected: " << inet_ntoa(clientaddr.sin_addr) << ":" << ntohs(clientaddr.sin_port) << std::endl;

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
		e.events = EPOLLIN | EPOLLRDHUP | EPOLLET;//ʱ�䣺�ɶ����Ҷϡ����ش���
		e.data.fd = newfd;
		if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, newfd, &e) == -1)//Ϊ�¼����������ע��
		{
			std::cout << "epoll_ctl error, fd =" << newfd << std::endl;
		}
	}

	std::cout << "exit accept thread." << std::endl;

	return NULL;
}

void* worker_thread_func(void* arg)
{
	std::cout << "worker thread is running." << std::endl;
	while (!g_bStop)
	{
		int clientfd;
		pthread_mutex_lock(&g_clientmutex);
		while (g_listClients.empty())
			pthread_cond_wait(&g_cond, &g_clientmutex);
		clientfd = g_listClients.front();//ȡ���������������
		g_listClients.pop_front();
		pthread_mutex_unlock(&g_clientmutex);

		if (g_bStop == true)break;
		//gdb����ʱ����ʵʱˢ�±�׼��������������ˢ�±�׼�����ʹ��Ϣ����Ļ��ʵʱ��ʾ����
		std::cout << std::endl;


		std::string strclientmsg;
		char buff[256];
		bool bError = false;
		while (true)
		{
			memset(buff, 0, sizeof(buff));
			int nRecv = recv(clientfd, buff, 256, 0);
			if (nRecv == -1)
			{
				if (errno == EWOULDBLOCK)//ֱ����Դ�����ã�˵�����ݽ������
					break;
				else
				{
					std::cout << "recv error, client disconnected, fd = " << clientfd << std::endl;
					release_client(clientfd);
					bError = true;
					break;
				}

			}
			//�Զ˹ر���socket�����Ҳ�رա�
			else if (nRecv == 0)
			{
				std::cout << "peer closed, client disconnected, fd = " << clientfd << std::endl;
				release_client(clientfd);
				bError = true;
				break;
			}

			strclientmsg += buff;
		}

		//�����ˣ��Ͳ�Ҫ�ټ�������ִ����
		if (bError)
			continue;

		std::cout << "client msg: " << strclientmsg;

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
			port = atol(optarg);
			break;
		}
	}

	if (bdaemon)
		daemon_run();

	if (port == 0)
		port = 12345;

	server_ip = get_local_ip_by_name(netcard_name);
	if (server_ip.empty() == true)
	{
		std::cout << "Unable to get server ip" << std::endl;
		return -1;
	}
	if (!create_server_listener(server_ip.c_str(), port))
	{
		std::cout << "Unable to create listen server: ip=0.0.0.0, port=" << port << "." << std::endl;
		return -1;
	}

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

	pthread_create(&g_acceptthreadid, NULL, accept_thread_func, NULL);

	//���������߳�
	for (int i = 0; i < WORKER_THREAD_NUM; ++i)
	{
		pthread_create(&g_threadid[i], NULL, worker_thread_func, NULL);
	}


	while (!g_bStop)
	{
		struct epoll_event ev[1024];
		int n = epoll_wait(g_epollfd, ev, 1024, 10);//10ms
		if (n == 0)//��ʱ
			continue;
		else if (n < 0)
		{
			std::cout << "epoll_wait error" << std::endl;
			continue;
		}

		int m = min(n, 1024);//���ֻ��ȡ��1024
		for (int i = 0; i < m; ++i)
		{
			//֪ͨ���������߳̽���������
			if (ev[i].data.fd == g_listenfd)
			{		
				g_accept_run_flag = true;
				pthread_cond_signal(&g_acceptcond);
			}
			//֪ͨ��ͨ�����߳̽�������
			else
			{
				pthread_mutex_lock(&g_clientmutex);
				g_listClients.push_back(ev[i].data.fd);
				pthread_mutex_unlock(&g_clientmutex);
				pthread_cond_signal(&g_cond);
				//std::cout << "signal" << std::endl;
			}

		}

	}

	return 0;

}