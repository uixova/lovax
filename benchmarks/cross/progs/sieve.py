N = 2000000
flags = [True]*(N+1)
count = 0
i = 2
while i <= N:
    if flags[i]:
        count += 1
        j = i*i
        while j <= N:
            flags[j] = False
            j += i
    i += 1
print(count)
