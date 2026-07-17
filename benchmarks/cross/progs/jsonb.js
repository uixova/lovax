const veri = [];
for (let i = 0; i < 500; i++) veri.push({id: i, ad: "oge", puan: i * 2, aktif: true});
let toplam = 0;
for (let k = 0; k < 100; k++) {
    const s = JSON.stringify(veri);
    const geri = JSON.parse(s);
    toplam += geri.length;
}
console.log(toplam);
