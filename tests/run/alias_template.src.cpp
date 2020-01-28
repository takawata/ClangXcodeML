#include <stdio.h>
template <typename T, int x>
struct A
{
  T a[x];
  A()
  {
    for(int i=0; i < x; i++){
      a[i] = 0;
    }
  }
};

struct D : public A<int , 3>{
  using BASE =  A<int, 3> ;
  
};

template <typename T>
using U3 = A<T, 3>;
using Uint3 = A<int, 3>;

template <typename T1, typename T2>
struct C{
  T1 x;
  T2 y;
};
template <int x> struct B{
  int u[x];
};
int main()
{
  U3<int> u;
  A<int ,4> a;
  B<3> b;
  C<int, double> c;
  D d;
  
  printf("Size%lu %lu %lu %lu\n", sizeof(u), sizeof(b) , sizeof(c) ,sizeof(d));
  
  return 0;
}
