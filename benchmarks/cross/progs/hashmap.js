const m = {};
for (let i=0;i<200000;i++) m["k"+i] = i;
let total = 0;
for (let i=0;i<200000;i++) total += m["k"+i];
console.log(total);
