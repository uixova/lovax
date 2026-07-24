local W,H,MAXI = 200,200,60
local total = 0
for py=0,H-1 do
  local y0 = py*2.5/H - 1.25
  for px=0,W-1 do
    local x0 = px*3.0/W - 2.0
    local x,y,i = 0.0,0.0,0
    while i < MAXI do
      local xx,yy = x*x, y*y
      if xx+yy > 4.0 then i = MAXI
      else
        local xt = xx-yy+x0
        y = 2.0*x*y + y0
        x = xt
        i = i + 1
        total = total + 1
      end
    end
  end
end
print(total)
