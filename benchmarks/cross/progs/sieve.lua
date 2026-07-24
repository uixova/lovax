local N = 2000000
local flags = {}
for i=0,N do flags[i]=true end
local count = 0
local i = 2
while i <= N do
  if flags[i] then
    count = count + 1
    local j = i*i
    while j <= N do flags[j]=false; j = j + i end
  end
  i = i + 1
end
print(count)
