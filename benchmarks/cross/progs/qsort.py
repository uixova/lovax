import sys
sys.setrecursionlimit(100000)
N = 300000
a = [0]*N
seed = 12345
for i in range(N):
    seed = seed * 16807 % 2147483647
    a[i] = seed % 1000000
def qsort(arr, lo, hi):
    if lo >= hi: return
    p = arr[(lo+hi)//2]
    i, j = lo, hi
    while i <= j:
        while arr[i] < p: i += 1
        while arr[j] > p: j -= 1
        if i <= j:
            arr[i], arr[j] = arr[j], arr[i]
            i += 1; j -= 1
    qsort(arr, lo, j); qsort(arr, i, hi)
qsort(a, 0, N-1)
print(a[0] + a[N//2] + a[N-1])
