local ents = {}
for i = 0, 19999 do ents[i+1] = {x=i*1.0, y=i*1.0, vx=1.5, vy=-0.5} end
for frame = 1, 200 do
  for j = 1, 20000 do local e = ents[j]; e.x = e.x + e.vx; e.y = e.y + e.vy end
end
local total = 0.0
for k = 1, 20000 do total = total + ents[k].x end
print(math.floor(total))
