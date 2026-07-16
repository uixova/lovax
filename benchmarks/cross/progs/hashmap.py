m = {}
for i in range(200000):
    m["k"+str(i)] = i
total = 0
for i in range(200000):
    total += m["k"+str(i)]
print(total)
