from datatable import dt, f, gcd

DT = dt.Frame(A=[1, 22, 3, 4, 5], B=[2, 33, 9, 16, 25])
RES = DT[:, gcd(f.A, f.B)]
RES

