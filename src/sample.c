typedef struct complex {
  double real;
  double img;
} complex_t;

complex_t x;
complex_t complex_add(complex_t x, double y);

main()
{
  complex_t z;

  x.real = 1.0;
  x.img = 2.0;

  z = complex_add(x,1.0);

  printf("z=(%f,%f)\n",z.real,z.img);
}

typedef struct dummy {
  int dummy_int;
  unsigned dummy_unsigned;
  double dummy_double;
} dummy_t;

complex_t complex_add(complex_t x, double y)
{
  x.real += y;
  return x;
}