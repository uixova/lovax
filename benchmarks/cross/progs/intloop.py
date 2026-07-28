def compute(n):
    acc = 0
    i = 0
    while i < n:
        acc = acc + (i * 3 + 7) % 13
        acc = acc % 30011
        i += 1
    return acc
print(compute(30000000))
