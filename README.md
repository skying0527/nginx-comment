# nginx-comment
nginx1.10.0源代码注释

2016-05-29  完成了对进程间通信和slab共享内存的注解。对其中一些具体的实现细节仍有不是很理解的地方，如在ngx_slab_alloc_pages函数中为什么要将剩余连续可用页的最后一页的prev指针指向剩余连续可用页的首页。

2016-06-19  此次主要是完成了Nginx内部变量部分的注解，主要包括内部变量的定义、初始化和使用；还完成了rewrite模块的set命令部分的 注解，主要包括set命令涉及到的脚本引擎相关的内容。另外，新增一个doc目录，主要是用于存放自己在学习源码过程中学到的一些东西，并将上次关于slab的一个总结文档也进行了上传。

2016-07-13  此次主要是完成了Nginx启动流程以及master和worker进程工作流程的注解。

2016-07-18  此次主要是完成了Nginx事件驱动框架部分的注解，包括处理流程以及如何解决负载均衡和“惊群”问题的实现。

2016-07-19  此次主要是完成了Nginx文件异步I/O处理流程的注解，包括Nginx如何实现异步文件I/O、eventfd和epoll的结合。

2016-08-03  此次主要是增加了关于Nginx的信号控制以及eventfd、异步I/O和epoll结合的总结，并建立了一个用来输出总结的个人博客，并将之前的总结和这两篇总结移植到博客中，博客地址为：https://TitenWang.github.io/

2016-08-20  此次主要是完成了Nginx中http框架的初始化流程中的大部分，主要包括listen和server_name命令的解析、Nginx中[port,ip]的端口管理模式以及多server监听同一个ip:port时如何定位真正的server块等内容。除此之外还包括了http框架如何管理http{}、server{}、location{}块下的配置项，
以及对请求的11个处理阶段的初始化