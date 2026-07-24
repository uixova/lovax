const W=200,H=200,MAXI=60;
let total=0;
for(let py=0;py<H;py++){
  const y0=py*2.5/H-1.25;
  for(let px=0;px<W;px++){
    const x0=px*3.0/W-2.0;
    let x=0.0,y=0.0,i=0;
    while(i<MAXI){
      const xx=x*x, yy=y*y;
      if(xx+yy>4.0){ i=MAXI; }
      else { const xt=xx-yy+x0; y=2.0*x*y+y0; x=xt; i++; total++; }
    }
  }
}
console.log(total);
