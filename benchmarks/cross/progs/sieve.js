const N=2000000;
const flags=new Array(N+1).fill(true);
let count=0,i=2;
while(i<=N){
  if(flags[i]){ count++; let j=i*i; while(j<=N){ flags[j]=false; j+=i; } }
  i++;
}
console.log(count);
