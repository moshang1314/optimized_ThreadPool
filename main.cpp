#include <thread>
#include <functional>
#include <future>
#include "threadpool.h"
#include <string>


int sum( int a , int b ) {
    std::this_thread::sleep_for( std::chrono::seconds( 2 ) );
    return a + b;
}
int main() {
    ThreadPool pool;
    pool.start( 1 );
    std::future<int> f = pool.submitTask( sum , 4 , 5 );
    std::future<int> f2 = pool.submitTask( sum , 4 , 6 );
    std::future<int> f3 = pool.submitTask( sum , 1 , 3 );
    std::cout << f.get() << std::endl;
    try {
        std::cout << f2.get() << std::endl;
    }
    catch (std::string s) {
        std::cout << s << std::endl;
    }

    try {
        std::cout << f3.get() << std::endl;
    } 
    catch (std::string s) {
        std::cout << s << std::endl;
    }
}