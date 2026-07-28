/* Worker side: echo the received message back to the parent. */
self.onmessage = function (e) {
    postMessage(e.data);
};
