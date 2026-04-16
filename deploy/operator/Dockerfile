FROM alpine:3.20
RUN apk add --no-cache bash curl jq \
    && curl -fsSL https://dl.k8s.io/release/v1.30.0/bin/linux/amd64/kubectl -o /usr/local/bin/kubectl \
    && chmod +x /usr/local/bin/kubectl \
    && curl -fsSL https://get.helm.sh/helm-v3.16.3-linux-amd64.tar.gz | tar xz -C /usr/local/bin --strip-components=1 linux-amd64/helm
COPY deploy/helm/zeptodb /opt/operator/chart
COPY deploy/operator/reconciler.sh /opt/operator/reconciler.sh
ENTRYPOINT ["/bin/bash", "/opt/operator/reconciler.sh"]
