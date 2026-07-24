const N=300000;
const a=new Array(N);
let seed=12345;
for(let i=0;i<N;i++){ seed=seed*16807%2147483647; a[i]=seed%1000000; }
function qsort(arr,lo,hi){
  if(lo>=hi) return;
  const p=arr[Math.floor((lo+hi)/2)];
  let i=lo,j=hi;
  while(i<=j){
    while(arr[i]<p) i++;
    while(arr[j]>p) j--;
    if(i<=j){ const t=arr[i]; arr[i]=arr[j]; arr[j]=t; i++; j--; }
  }
  qsort(arr,lo,j); qsort(arr,i,hi);
}
qsort(a,0,N-1);
console.log(a[0]+a[Math.floor(N/2)]+a[N-1]);
