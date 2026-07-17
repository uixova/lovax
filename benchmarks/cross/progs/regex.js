let toplam = 0;
for (let i = 0; i < 3000; i++) {
    const bulunan = "user1 kai@lovax.dev x99 mail: eda@oyun.io ve son: a1b2c3 v12".match(/[a-z]+\d+/g) || [];
    toplam += bulunan.length;
}
console.log(toplam);
