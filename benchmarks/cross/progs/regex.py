import re
toplam = 0
for _ in range(3000):
    bulunan = re.findall(r"[a-z]+\d+", "user1 kai@lovax.dev x99 mail: eda@oyun.io ve son: a1b2c3 v12")
    toplam += len(bulunan)
print(toplam)
