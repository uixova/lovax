local N = 300000
local a = {}
local seed = 12345
for i=0,N-1 do
  seed = seed * 16807 % 2147483647
  a[i] = seed % 1000000
end
local function qsort(arr, lo, hi)
  if lo >= hi then return end
  local p = arr[math.floor((lo+hi)/2)]
  local i, j = lo, hi
  while i <= j do
    while arr[i] < p do i = i + 1 end
    while arr[j] > p do j = j - 1 end
    if i <= j then arr[i], arr[j] = arr[j], arr[i]; i = i + 1; j = j - 1 end
  end
  qsort(arr, lo, j); qsort(arr, i, hi)
end
qsort(a, 0, N-1)
print(a[0] + a[math.floor(N/2)] + a[N-1])
