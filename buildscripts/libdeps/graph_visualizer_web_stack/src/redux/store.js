import { createStore, combineReducers } from "redux";
import { nodes } from "./nodes";
import { graphFiles } from "./graphFiles";
import { counts } from "./counts";
import { nodeInfo } from "./nodeInfo";
import { loading } from "./loading";
import { links } from "./links";
import { graphData } from "./graphData";
import { findNode } from "./findNode";
import { graphPaths } from "./graphPaths";
import { listSearchTerm } from "./listSearchTerm";

export const initialState = {
  loading: false,
  graphFiles: [
    // {id: 0, value: 'graphfile.graphml', version: 1, git: '1234567', selected: false}
  ],
  nodes: [
    {
      id: 0,
      node: "test/test1.so",
      name: "test1",
      check: "checkbox",
      selected: false,
    },
    {
      id: 1,
      node: "test/test2.so",
      name: "test2",
      check: "checkbox",
      selected: false,
    },
  ],
  links: [{ source: "test/test1.so", target: "test/test2.so" }],
  graphData: {
    nodes: [
      // {id: 'test/test1.so', name: 'test1.so'},
      // {id: 'test/test2.so', name: 'test2.so'}
    ],
    links: [
      // {source: 'test/test1.so', target: 'test/test2.so'}
    ],
  },
  graphPaths: {
    fromNode: "test",
    toNode: "test",
    paths: [
      ["test1", "test2"],
      ["test1", "test3", "test2"],
    ],
    selectedPath: -1,
  },
  counts: [{ id: 0, type: "node2", value: 0 }],
  findNode: "",
  nodeInfo: [
    {
      id: 0,
      node: "test/test.so",
      name: "test",
      attribs: [{ name: "test", value: "test" }],
      dependers: [{ node: "test/test3.so", symbols: [] }],
      dependencies: [{ node: "test/test2.so", symbols: [] }],
    },
  ],
  listSearchTerm: "",
};

export const getLoading = (state) => {
  return { loading: state };
};

export const getGraphFiles = (state) => {
  return {
    loading: state.loading,
    graphFiles: state.graphFiles,
  };
};

export const getNodeInfos = (state) => {
  return {
    nodeInfos: state.nodeInfo,
  };
};

export const getCounts = (state) => {
  const counts = state.counts;
  return {
    counts: state.counts,
  };
};

export const getRows = (state) => {
  let searchedNodes = state.nodes.filter(node => node.node.indexOf(state.listSearchTerm) > -1);
  return {
    rowCount: searchedNodes.length,
    rowGetter: ({ index }) => searchedNodes[index],
    checkBox: ({ index }) => searchedNodes[index].selected,
    nodes: searchedNodes,
  };
};

export const getSelected = (state) => {
  return {
    selectedNodes: state.nodes.filter((node) => node.selected),
    selectedEdges: [],
    loading: state.loading,
    graphPaths: state.graphPaths,
  };
};

export const getNodes = (state) => {
  return {
    nodes: state.nodes,
    loading: state.loading,
    listSearchTerm: state.listSearchTerm,
    searchedNodes: state.nodes.filter(node => node.node.indexOf(state.listSearchTerm) > -1),
  };
};

export const getGraphData = (state) => {
  return {
    nodes: state.nodes,
    graphData: state.graphData,
    loading: state.loading,
    findNode: state.findNode,
    graphPaths: state.graphPaths,
  };
};

export const getFullState = (state) => {
  return { state };
};

const store = createStore(
  combineReducers({
    nodes,
    counts,
    nodeInfo,
    graphFiles,
    loading,
    links,
    graphData,
    findNode,
    graphPaths,
    listSearchTerm,
  }),
  initialState
);
export default store;
