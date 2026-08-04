const ents=[]; for(let i=0;i<20000;i++)ents.push({x:i,y:i,vx:1.5,vy:-0.5});
for(let f=0;f<200;f++){for(let j=0;j<20000;j++){const e=ents[j];e.x+=e.vx;e.y+=e.vy;}}
let t=0; for(let k=0;k<20000;k++)t+=ents[k].x; console.log(Math.floor(t));
