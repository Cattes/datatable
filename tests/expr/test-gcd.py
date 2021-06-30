from datatable import dt, f, gcd

DT = dt.Frame(A=[1, 2, 3, 4, 5])
RES = DT[:, gcd(f.A, f.A)]


