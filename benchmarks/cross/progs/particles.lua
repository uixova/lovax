local total = 0.0
for frame = 1, 400 do
  local parts = {}
  for i = 0, 1999 do parts[i+1] = {x=i*1.0, y=i*0.5, life=60} end
  for j = 1, 2000 do total = total + parts[j].x end
end
print(math.floor(total))
