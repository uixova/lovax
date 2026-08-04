const xs=[],ys=[],vxs=[],vys=[];for(let i=0;i<20000;i++){xs.push(i);ys.push(i);vxs.push(1.5);vys.push(-0.5);}
for(let f=0;f<200;f++)for(let j=0;j<20000;j++){xs[j]+=vxs[j];ys[j]+=vys[j];}
let t=0;for(let k=0;k<20000;k++)t+=xs[k];console.log(Math.floor(t));
