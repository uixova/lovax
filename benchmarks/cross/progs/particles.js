let total=0.0;
for(let f=0;f<400;f++){const parts=[];for(let i=0;i<2000;i++)parts.push({x:i*1.0,y:i*0.5,life:60});for(let j=0;j<2000;j++)total+=parts[j].x;}
console.log(Math.floor(total));
