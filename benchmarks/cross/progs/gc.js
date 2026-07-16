let sink = 0;
for (let i=0;i<300000;i++){ const tmp=[i,i+1,i+2]; sink += tmp[0]; }
console.log(sink);
