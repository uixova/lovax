local xs,ys,vxs,vys={},{},{},{}
for i=0,19999 do xs[i+1]=i*1.0; ys[i+1]=i*1.0; vxs[i+1]=1.5; vys[i+1]=-0.5 end
for f=1,200 do for j=1,20000 do xs[j]=xs[j]+vxs[j]; ys[j]=ys[j]+vys[j] end end
local t=0.0; for k=1,20000 do t=t+xs[k] end; print(math.floor(t))
