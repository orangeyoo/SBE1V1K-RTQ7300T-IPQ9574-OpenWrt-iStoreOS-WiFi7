-- Copyright 2022 xiaobao <xiaobao@linkease.com>
-- Licensed to the public under the MIT License

local http = require "luci.http"
local nixio = require "nixio"
local ltn12 = require "luci.ltn12"
local table = require "table"
local util = require "luci.util"

module("luci.controller.istore_backend", package.seeall)

local BLOCKSIZE = 2048
local ISTOREOS_PORT = 3038

function index()
    entry({"istore"}, call("istore_backend")).leaf=true
end

local function sink_socket(sock, io_err)
  if sock then
    return function(chunk, err)
      if not chunk then
        return 1
      else
        return sock:send(chunk)
      end
    end
  else
    return ltn12.sink.error(io_err or "unable to send socket")
  end
end

local function session_retrieve(sid, allowed_users)
  local sdat = util.ubus("session", "get", { ubus_rpc_session = sid })
  if type(sdat) == "table" and
      type(sdat.values) == "table" and
      type(sdat.values.token) == "string" and
      (not allowed_users or
      util.contains(allowed_users, sdat.values.username))
  then
      return sid, sdat.values
  end
  return nil, nil
end

local function get_session()
  local sid
  local key
  local sdat
  for _, key in ipairs({"sysauth_https", "sysauth_http", "sysauth"}) do
    sid = http.getcookie(key)
    if sid then
      sid, sdat = session_retrieve(sid, nil)
      if sid and sdat then
        return sid, sdat
      end
    end
  end
  return nil, nil
end

local function chunksource(sock, buffer)
	buffer = buffer or ""
	return function()
		local output
		local _, endp, count = buffer:find("^([0-9a-fA-F]+);?.-\r\n")
		while not count and #buffer <= 1024 do
			local newblock, code = sock:recv(1024 - #buffer)
			if not newblock then
				return nil, code
			end
			buffer = buffer .. newblock
			_, endp, count = buffer:find("^([0-9a-fA-F]+);?.-\r\n")
		end
		count = tonumber(count, 16)
		if not count then
			return nil, -1, "invalid encoding"
		elseif count == 0 then
			return nil
		elseif count + 2 <= #buffer - endp then
			output = buffer:sub(endp+1, endp+count)
			buffer = buffer:sub(endp+count+3)
			return output
		else
			output = buffer:sub(endp+1, endp+count)
			buffer = ""
			if count - #output > 0 then
				local remain, code = sock:recvall(count-#output)
				if not remain then
					return nil, code
				end
				output = output .. remain
				count, code = sock:recvall(2)
			else
				count, code = sock:recvall(count+2-#buffer+endp)
			end
			if not count then
				return nil, code
			end
			return output
		end
	end
end

-- SBE1V1K board stubs: eMMC has no parted-exported disk tree and dockerd is
-- optional, so answer these two wizard endpoints locally instead of proxying
-- (the real backend hangs on docker status without dockerd running).
local function stub_docker_status()
  local dr = "/opt/docker/"
  local f1 = io.popen("uci -q get dockerd.globals.data_root")
  if f1 then
    local v = f1:read("*l")
    f1:close()
    if v and #v > 1 then dr = v end
  end
  local st = "stopped"
  local f2 = io.popen("pgrep dockerd 2>/dev/null")
  if f2 then
    local pid = f2:read("*l")
    f2:close()
    if pid and #pid > 0 then st = "running" end
  end
  return '{"result":{"path":"' .. dr .. '","status":"' .. st .. '"}}'
end

local function stub_disk_status()
  local total, used, avail, pc = 0, 0, 0, 0
  local dev = "mmcblk0p28"
  local f3 = io.popen("df -k / 2>/dev/null")
  if f3 then
    f3:read("*l")
    local row = f3:read("*l")
    f3:close()
    if row then
      local d2, t2, u2, a2, p2 = row:match("^(%S+)%s+(%d+)%s+(%d+)%s+(%d+)%s+(%d+)%%%s+")
      if t2 then
        dev = d2:gsub("^/dev/", ""):gsub("overlayfs:.*", "mmcblk0p28")
        total = math.floor(tonumber(t2) or 0)
        used = math.floor(tonumber(u2) or 0)
        avail = math.floor(tonumber(a2) or 0)
        pc = math.floor(tonumber(p2) or 0)
      end
    end
  end
  local function hkb(kb)
    if kb >= 1073741824 then return string.format("%.1fTB", kb / 1073741824) end
    if kb >= 1048576 then return string.format("%.1fGB", kb / 1048576) end
    if kb >= 1024 then return string.format("%.1fMB", kb / 1024) end
    return string.format("%.1fKB", kb)
  end
  local mp = {
    mountPoint = "/",
    path = "/dev/" .. dev,
    name = dev,
    filesystem = "ext4",
    total = hkb(total),
    used = hkb(used),
    usage = pc,
    uuid = "root-overlay-" .. dev,
    isSystemRoot = true
  }
  local disk = {
    name = dev,
    venderModel = "eMMC 8GB",
    path = "/dev/" .. dev,
    total = hkb(total),
    size = hkb(total),
    used = hkb(used),
    usage = pc,
    childrens = { mp },
    isSystemRoot = true,
    isDockerRoot = true
  }
  local parts = {}
  local function jstr(s)
    return '"' .. tostring(s):gsub('\\', '\\\\'):gsub('"', '\\"') .. '"'
  end
  parts[#parts + 1] = '{"result":{"disks":[{"name":' .. jstr(disk.name)
  parts[#parts + 1] = ',"venderModel":' .. jstr(disk.venderModel)
  parts[#parts + 1] = ',"path":' .. jstr(disk.path)
  parts[#parts + 1] = ',"total":' .. jstr(disk.total)
  parts[#parts + 1] = ',"size":' .. jstr(disk.size)
  parts[#parts + 1] = ',"used":' .. jstr(disk.used)
  parts[#parts + 1] = ',"usage":' .. tostring(disk.usage)
  parts[#parts + 1] = ',"childrens":[{"mountPoint":' .. jstr(mp.mountPoint)
  parts[#parts + 1] = ',"path":' .. jstr(mp.path)
  parts[#parts + 1] = ',"name":' .. jstr(mp.name)
  parts[#parts + 1] = ',"filesystem":' .. jstr(mp.filesystem)
  parts[#parts + 1] = ',"total":' .. jstr(mp.total)
  parts[#parts + 1] = ',"used":' .. jstr(mp.used)
  parts[#parts + 1] = ',"usage":' .. tostring(mp.usage)
  parts[#parts + 1] = ',"uuid":' .. jstr(mp.uuid)
  parts[#parts + 1] = ',"isSystemRoot":true}'
  parts[#parts + 1] = '],"isSystemRoot":true,"isDockerRoot":true}]}}'
  return table.concat(parts)
end

function istore_backend()
  local pre_uri = http.getenv("REQUEST_URI") or ""
  if string.find(pre_uri, "nas/disk/status", 1, true) then
    http.status(200, "OK")
    http.header("Content-Type", "application/json")
    http.write(stub_disk_status())
    return
  end
  if string.find(pre_uri, "guide/docker/status", 1, true) then
    http.status(200, "OK")
    http.header("Content-Type", "application/json")
    http.write(stub_docker_status())
    return
  end

  local sock = nixio.connect("127.0.0.1", ISTOREOS_PORT)
  if not sock then
    http.status(500, "connect failed")
    return
  end
  local input = {}
  input[#input+1] = http.getenv("REQUEST_METHOD") .. " " .. http.getenv("REQUEST_URI") .. " HTTP/1.1"
  local req = http.context.request
  local start = "HTTP_"
  local start_len = string.len(start)
  local ctype = http.getenv("CONTENT_TYPE")
  if ctype then
    input[#input+1] = "Content-Type: " .. ctype
  end
  for k, v in pairs(req.message.env) do
    if string.sub(k, 1, start_len) == start and not string.find(k, "FORWARDED") then
      input[#input+1] = string.sub(k, start_len+1, string.len(k)) .. ": " .. v
    end
  end
  local sid, sdat = get_session()
  if sdat ~= nil then
    input[#input+1] = "X-Forwarded-Sid: " .. sid
    input[#input+1] = "X-Forwarded-Token: " .. sdat.token
  end
  -- input[#input+1] = "X-Forwarded-For: " .. http.getenv("REMOTE_HOST") ..":".. http.getenv("REMOTE_PORT")
  local num = tonumber(http.getenv("CONTENT_LENGTH")) or 0
  input[#input+1] = "Content-Length: " .. tostring(num)
  input[#input+1] = "\r\n"
  local source = ltn12.source.cat(ltn12.source.string(table.concat(input, "\r\n")), http.source())
  local ret = ltn12.pump.all(source, sink_socket(sock, "write sock error"))
  if ret ~= 1 then
    sock:close()
    http.status(500, "proxy error")
    return
  end

  local linesrc = sock:linesource()
  local line, code, error = linesrc()
  if not line then
    sock:close()
    http.status(500, "response parse failed")
    return
  end

  local protocol, status, msg = line:match("^([%w./]+) ([0-9]+) (.*)")
  if not protocol then
    sock:close()
    http.status(500, "response protocol error")
    return
  end
  num = tonumber(status) or 0
  http.status(num, msg)

  local chunked = 0
  line = linesrc()
  while line and line ~= "" do
    local key, val = line:match("^([%w-]+)%s?:%s?(.*)")
    if key and key ~= "Status" then
      if key == "Transfer-Encoding" and val == "chunked" then
        chunked = 1
      end
      if key ~= "Connection" and key ~= "Transfer-Encoding" and key ~= "Content-Length" then
        http.header(key, val)
      end
    end
    line = linesrc()
  end
  if not line then
    sock:close()
    http.status(500, "parse header failed")
    return
  end

  local body_buffer = linesrc(true)
  if chunked == 1 then
    ltn12.pump.all(chunksource(sock, body_buffer), http.write)
  else
    local body_source = ltn12.source.cat(ltn12.source.string(body_buffer), sock:blocksource())
    ltn12.pump.all(body_source, http.write)
  end

  sock:close()
end
