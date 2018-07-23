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

int g_epollfd = 0;//epoll句柄
bool g_bStop = false;//应用退出标志
bool g_accept_run_flag = false;//accept触发标志
int g_listenfd = 0;//监听描述符


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
	pthread_cond_signal(&g_acceptcond);//通知accept线程退出
	pthread_join(g_acceptthreadid, NULL);//等待回收线程

	struct epoll_event ev;
	ev.data.fd = 0;
	pthread_mutex_lock(&g_clientmutex);//通知worker线程退出
	g_listClients.push_back(ev.data.fd);
	g_listClients.push_back(ev.data.fd);
	pthread_mutex_unlock(&g_clientmutex);
	pthread_cond_broadcast(&g_cond);
	pthread_join(g_threadid[0], NULL);
	pthread_join(g_threadid[1], NULL);//等待回收线程

	epoll_ctl(g_epollfd, EPOLL_CTL_DEL, g_listenfd, NULL);//删除监听描述符

	//TODO: 是否需要先调用shutdown()一下？
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

	return std::string(ip);

}

bool create_server_listener(const char* ip, short port)
{
	g_listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);//创建socket
	if (g_listenfd == -1)
		return false;

	int on = 1;
	setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));//重用地址与端口
	setsockopt(g_listenfd, SOL_SOCKET, SO_REUSEPORT, (char *)&on, sizeof(on));

	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(ip);
	servaddr.sin_port = htons(port);
	if (bind(g_listenfd, (sockaddr *)&servaddr, sizeof(servaddr)) == -1)//绑定端口与IP
		return false;

	if (listen(g_listenfd, 50) == -1)//设置最大监听数量
		return false;

	g_epollfd = epoll_create(1);//创建一个epoll句柄
	if (g_epollfd == -1)
		return false;

	struct epoll_event e;
	memset(&e, 0, sizeof(e));
	e.events = EPOLLIN | EPOLLRDHUP;//注册事件：可读/关闭，挂断
	e.data.fd = g_listenfd;
	if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_listenfd, &e) == -1)//注册新的监听fd到g_epollfd。
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
		while (g_accept_run_flag == false)//防止意外唤醒
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
		e.events = EPOLLIN | EPOLLRDHUP | EPOLLET;//时间：可读、挂断、边沿触发
		e.data.fd = newfd;
		if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, newfd, &e) == -1)//为新加入的描述符注册
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
		clientfd = g_listClients.front();//取出被激活的描述符
		g_listClients.pop_front();
		pthread_mutex_unlock(&g_clientmutex);

		if (g_bStop == true)break;
		//gdb调试时不能实时刷新标准输出，用这个函数刷新标准输出，使信息在屏幕上实时显示出来
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
				if (errno == EWOULDBLOCK)//直到资源不可用，说明数据接收完成
					break;
				else
				{
					std::cout << "recv error, client disconnected, fd = " << clientfd << std::endl;
					release_client(clientfd);
					bError = true;
					break;
				}

			}
			//对端关闭了socket，这端也关闭。
			else if (nRecv == 0)
			{
				std::cout << "peer closed, client disconnected, fd = " << clientfd << std::endl;
				release_client(clientfd);
				bError = true;
				break;
			}

			strclientmsg += buff;
		}

		//出错了，就不要再继续往下执行了
		if (bError)
			continue;

		std::cout << "client msg: " << strclientmsg;

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

	//设置信号处理
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

	//启动工作线程
	for (int i = 0; i < WORKER_THREAD_NUM; ++i)
	{
		pthread_create(&g_threadid[i], NULL, worker_thread_func, NULL);
	}


	while (!g_bStop)
	{
		struct epoll_event ev[1024];
		int n = epoll_wait(g_epollfd, ev, 1024, 10);//10ms
		if (n == 0)//超时
			continue;
		else if (n < 0)
		{
			std::cout << "epoll_wait error" << std::endl;
			continue;
		}

		int m = min(n, 1024);//最大只能取到1024
		for (int i = 0; i < m; ++i)
		{
			//通知接收连接线程接收新连接
			if (ev[i].data.fd == g_listenfd)
			{		
				g_accept_run_flag = true;
				pthread_cond_signal(&g_acceptcond);
			}
			//通知普通工作线程接收数据
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