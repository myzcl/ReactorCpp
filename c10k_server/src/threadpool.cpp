#include "threadpool.h"


threadpool_t ThreadPool::m_pool;


ThreadPool::ThreadPool(int threadNum, void *(*run)(void *arg), void *arg)
{
    //初始化线程池，最多threadNum个线程
    threadpool_init(&m_pool, threadNum);
    for (int i = 0; i < threadNum; i++)
    {
        threadpool_add_task(&m_pool, run, arg);
    }
}
ThreadPool::~ThreadPool()
{
    threadpool_destroy();
}



/******************************线程池状态********************************/
//初始化
int ThreadPool::condition_init(condition_t *cond)
{
    int status;
    if((status = pthread_mutex_init(&cond->pmutex, NULL)))
        return status;

    if((status = pthread_cond_init(&cond->pcond, NULL)))
        return status;

    return 0;
}

//加锁
int ThreadPool::condition_lock(condition_t *cond)
{
    return pthread_mutex_lock(&cond->pmutex);
}

//解锁
int ThreadPool::condition_unlock(condition_t *cond)
{
    return pthread_mutex_unlock(&cond->pmutex);
}

//pthread_cond_wait和pthread_cond_timedwait用来等待条件变量被设置，值得注意的是这两个等待调用需要一个已经上锁的互斥体mutex
//等待
int ThreadPool::condition_wait(condition_t *cond)
{
    return pthread_cond_wait(&cond->pcond, &cond->pmutex);
}

//固定时间等待
int ThreadPool::condition_timedwait(condition_t *cond, const struct timespec *abstime)
{
    return pthread_cond_timedwait(&cond->pcond, &cond->pmutex, abstime);
}

//pthread_cond_signal则用于解除某一个等待线程的阻塞状态
//唤醒一个睡眠线程
int ThreadPool::condition_signal(condition_t* cond)
{
    return pthread_cond_signal(&cond->pcond);
}

//pthread_cond_broadcast用于设置条件变量，即 使得事件发生，这样等待该事件的线程将不再阻塞
//唤醒所有睡眠线程
int ThreadPool::condition_broadcast(condition_t *cond)
{
    return pthread_cond_broadcast(&cond->pcond);
}

//释放
int ThreadPool::condition_destroy(condition_t *cond)
{
    int status;
    if((status = pthread_mutex_destroy(&cond->pmutex)))
        return status;

    if((status = pthread_cond_destroy(&cond->pcond)))
        return status;

    return 0;
}
/******************************线程池状态********************************/


/******************************线程池操作********************************/
//创建的线程执行
void *ThreadPool::thread_routine(void *arg)
{
    struct timespec abstime;
    int timeout;
    printf("thread %d is starting\n", (int)pthread_self());
    threadpool_t *pool = (threadpool_t *)arg;
    while(1)
    {
        timeout = 0;
        //访问线程池之前需要加锁
        condition_lock(&pool->ready);
        //空闲进程
        pool->idle++;
        //等待队列有任务到来 或者 收到线程池销毁通知
        while(pool->first == NULL && !pool->quit)
        {
            //否则线程阻塞等待
            printf("thread %d is waiting\n", (int)pthread_self());
            //获取从当前时间，并加上等待时间， 设置进程的超时睡眠时间
            clock_gettime(CLOCK_REALTIME, &abstime);
            abstime.tv_sec += 4;	//线程等待4s后，自动退出
            int status;
            status = condition_timedwait(&pool->ready, &abstime);  //该函数会解锁，允许其他线程访问，当被唤醒时，加锁

            if(status == ETIMEDOUT)
            {
                printf("thread %d wait timed out\n", (int)pthread_self());
                timeout = 1;
                break;
            }
        }

        pool->idle--;
        if(pool->first != NULL)
        {
            //取出等待队列最前的任务，移除任务，并执行任务
            task_t *t = pool->first;
            pool->first = t->next;
            //由于任务执行需要消耗时间，先解锁让其他线程访问线程池
            condition_unlock(&pool->ready);
            //执行任务
            t->run(t->arg);
            //执行完任务释放内存
            free(t);
            //重新加锁
            condition_lock(&pool->ready);
        }

        //退出线程池
        if(pool->quit && pool->first == NULL)
        {
            pool->counter--;//当前工作的线程数-1
            //若线程池中没有线程，通知等待线程（主线程）全部任务已经完成
            if(pool->counter == 0)
            {
                condition_signal(&pool->ready);
            }
            condition_unlock(&pool->ready);
            break;
        }

        //超时，跳出销毁线程
        if(timeout == 1)
        {
            pool->counter--;//当前工作的线程数-1
            condition_unlock(&pool->ready);
            break;
        }


        condition_unlock(&pool->ready);
    }

    printf("thread %d is exiting\n", (int)pthread_self());
    return NULL;

}


//线程池初始化
void ThreadPool::threadpool_init(threadpool_t *pool, int threads)
{
    condition_init(&pool->ready);
    pool->first = NULL;
    pool->last =NULL;
    pool->counter =0;
    pool->idle =0;
    pool->max_threads = threads;
    pool->quit =0;
}


//增加一个任务到线程池
void ThreadPool::threadpool_add_task(threadpool_t *pool, void *(*run)(void *arg), void *arg)
{
    //产生一个新的任务
    task_t *newtask = (task_t *)malloc(sizeof(task_t));
    newtask->run = run;
    newtask->arg = arg;
    newtask->next=NULL;//新加的任务放在队列尾端

    //线程池的状态被多个线程共享，操作前需要加锁
    condition_lock(&pool->ready);

    if(pool->first == NULL)//第一个任务加入
    {
        pool->first = newtask;
    }
    else
    {
        pool->last->next = newtask;
    }
    pool->last = newtask;  //队列尾指向新加入的线程

    //线程池中有线程空闲，唤醒
    if(pool->idle > 0)
    {
        condition_signal(&pool->ready);
    }
    //当前线程池中线程个数没有达到设定的最大值，创建一个新的线性
    else if(pool->counter < pool->max_threads)
    {
        pthread_t tid;
        pthread_create(&tid, NULL, thread_routine, pool);
        pool->counter++;
    }
    //结束，访问
    condition_unlock(&pool->ready);
}

//线程池销毁
void ThreadPool::threadpool_destroy()
{
    //如果已经调用销毁，直接返回
    if(m_pool.quit)
    {
        return;
    }
    //加锁
    condition_lock(&m_pool.ready);
    //设置销毁标记为1
    m_pool.quit = 1;
    //线程池中线程个数大于0
    if(m_pool.counter > 0)
    {
        //对于等待的线程，发送信号唤醒
        if(m_pool.idle > 0)
        {
            condition_broadcast(&m_pool.ready);
        }
        //正在执行任务的线程，等待他们结束任务
        while(m_pool.counter)
        {
            condition_wait(&m_pool.ready);
        }
    }
    condition_unlock(&m_pool.ready);
    condition_destroy(&m_pool.ready);
}

/******************************线程池操作********************************/


