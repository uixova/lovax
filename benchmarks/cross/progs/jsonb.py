import json
veri = [{"id": i, "ad": "oge", "puan": i * 2, "aktif": True} for i in range(500)]
toplam = 0
for _ in range(100):
    s = json.dumps(veri)
    geri = json.loads(s)
    toplam += len(geri)
print(toplam)
