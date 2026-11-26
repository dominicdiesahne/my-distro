-- my-distro (1.3.0): sound for programs without the microphone permission.
--
-- my-run gives such a program only the socket pipewire-0-play, and PipeWire
-- marks every client of that socket with pipewire.access = "my-play"
-- (/usr/share/pipewire/pipewire.conf.d/50-my-play.conf; a program can't
-- change that property). Those clients may play sound and nothing else:
--
--   1. They see only their own nodes. PipeWire lets a client link two nodes
--      only if it sees both, so it can't connect itself to the microphone,
--      to the monitor of the speakers or to another program's sound.
--   2. A node of theirs that isn't a playback stream (a capture stream, a
--      virtual sink or source) is removed.
--   3. WirePlumber never links into one of their nodes, and a link that ends
--      in one anyway is removed.

log = Log.open_topic ("s-client")

nodes_om = ObjectManager { Interest { type = "node" } }
clients_om = ObjectManager { Interest { type = "client" } }
links_om = ObjectManager { Interest { type = "link" } }

local function by_id (om, id)
  if id == nil then
    return nil
  end
  return om:lookup { Constraint { "bound-id", "=", id, type = "gobject" } }
end

local function owner_id (node)
  return tonumber (node.properties ["client.id"])
end

local function is_play (client)
  return client ~= nil and client.properties ["pipewire.access"] == "my-play"
end

local function play_client (id)
  local client = by_id (clients_om, id)
  if is_play (client) then
    return client
  end
  return nil
end

-- 2.: only playback streams
local function check_node (node)
  local class = node.properties ["media.class"] or ""
  if not class:find ("^Stream/Output/") then
    log:notice (node, "removing node " .. node ["bound-id"] .. " (" .. class ..
        ") of a program without the microphone permission")
    node:request_destroy ()
  end
end

-- 1.: hide the nodes of others
local function hide_nodes (client)
  local id = client ["bound-id"]
  client:update_permissions { ["any"] = "rx" }
  local deny = {}
  local n = 0
  for node in nodes_om:iterate () do
    if owner_id (node) == id then
      check_node (node)
    else
      deny [node ["bound-id"]] = "-"
      n = n + 1
    end
  end
  if n > 0 then
    client:update_permissions (deny)
  end
  log:info (client, "client " .. id .. " may only play sound (" .. n .. " nodes hidden)")
end

clients_om:connect ("object-added", function (om, client)
  if is_play (client) then
    hide_nodes (client)
  end
end)

nodes_om:connect ("object-added", function (om, node)
  local nid = node ["bound-id"]
  local oid = owner_id (node)
  if play_client (oid) then
    check_node (node)
  end
  for client in clients_om:iterate () do
    if is_play (client) and client ["bound-id"] ~= oid then
      client:update_permissions { [nid] = "-" }
    end
  end
end)

-- 3.: no data into their nodes
links_om:connect ("object-added", function (om, link)
  local node = by_id (nodes_om, tonumber (link.properties ["link.input.node"]))
  if node ~= nil and play_client (owner_id (node)) then
    log:notice (link, "removing a link into node " .. node ["bound-id"] ..
        " of a program without the microphone permission")
    link:request_destroy ()
  end
end)

SimpleEventHook {
  name = "linking/my-play-only",
  before = "linking/prepare-link",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-target" },
    },
  },
  execute = function (event)
    local si = event:get_subject ()
    if si.properties ["item.node.direction"] ~= "input" then
      return
    end
    local node = si:get_associated_proxy ("node")
    if node ~= nil and play_client (owner_id (node)) then
      log:notice (si, "not linking into a program without the microphone permission")
      event:stop_processing ()
    end
  end
}:register ()

nodes_om:activate ()
clients_om:activate ()
links_om:activate ()
