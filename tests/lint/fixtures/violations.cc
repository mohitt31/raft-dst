// Every non-blank code line below must be flagged by banned_apis.sh.
#include <chrono>
#include <thread>
#include <random>
#include <mutex>
int a = rand();
int b = std::rand();
long c = time(nullptr);
const char* d = getenv("X");
double e = 1;
float f = 2;
std::uniform_int_distribution<int> g(0, 9);
std::shuffle(v.begin(), v.end(), rng);
std::mt19937 h;
size_t i = std::hash<int>{}(3);
