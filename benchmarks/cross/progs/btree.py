import sys
sys.setrecursionlimit(100000)
def make(d):
    if d == 0: return (1, None, None)
    return (1, make(d-1), make(d-1))
def check(n):
    if n[1] is None: return n[0]
    return n[0] + check(n[1]) + check(n[2])
total = 0
for i in range(8):
    total += check(make(15))
print(total)
