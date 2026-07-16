sink = 0
for i in range(300000):
    tmp = [i, i+1, i+2]
    sink += tmp[0]
print(sink)
